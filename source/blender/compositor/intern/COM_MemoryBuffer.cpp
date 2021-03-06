/*
 * Copyright 2011, Blender Foundation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor: 
 *		Jeroen Bakker 
 *		Monique Dewanchand
 */

#include "COM_MemoryBuffer.h"
#include "MEM_guardedalloc.h"
//#include "BKE_global.h"

unsigned int MemoryBuffer::determineBufferSize()
{
	return getWidth() * getHeight();
}

int MemoryBuffer::getWidth() const
{
	return this->m_rect.xmax - this->m_rect.xmin;
}
int MemoryBuffer::getHeight() const
{
	return this->m_rect.ymax - this->m_rect.ymin;
}

MemoryBuffer::MemoryBuffer(MemoryProxy *memoryProxy, unsigned int chunkNumber, rcti *rect)
{
	BLI_rcti_init(&this->m_rect, rect->xmin, rect->xmax, rect->ymin, rect->ymax);
	this->m_memoryProxy = memoryProxy;
	this->m_chunkNumber = chunkNumber;
	this->m_buffer = (float *)MEM_mallocN(sizeof(float) * determineBufferSize() * COM_NUMBER_OF_CHANNELS, "COM_MemoryBuffer");
	this->m_state = COM_MB_ALLOCATED;
	this->m_datatype = COM_DT_COLOR;
	this->m_chunkWidth = this->m_rect.xmax - this->m_rect.xmin;
}

MemoryBuffer::MemoryBuffer(MemoryProxy *memoryProxy, rcti *rect)
{
	BLI_rcti_init(&this->m_rect, rect->xmin, rect->xmax, rect->ymin, rect->ymax);
	this->m_memoryProxy = memoryProxy;
	this->m_chunkNumber = -1;
	this->m_buffer = (float *)MEM_mallocN(sizeof(float) * determineBufferSize() * COM_NUMBER_OF_CHANNELS, "COM_MemoryBuffer");
	this->m_state = COM_MB_TEMPORARILY;
	this->m_datatype = COM_DT_COLOR;
	this->m_chunkWidth = this->m_rect.xmax - this->m_rect.xmin;
}
MemoryBuffer *MemoryBuffer::duplicate()
{
	MemoryBuffer *result = new MemoryBuffer(this->m_memoryProxy, &this->m_rect);
	memcpy(result->m_buffer, this->m_buffer, this->determineBufferSize() * COM_NUMBER_OF_CHANNELS * sizeof(float));
	return result;
}
void MemoryBuffer::clear()
{
	memset(this->m_buffer, 0, this->determineBufferSize() * COM_NUMBER_OF_CHANNELS * sizeof(float));
}

float *MemoryBuffer::convertToValueBuffer()
{
	const unsigned int size = this->determineBufferSize();
	unsigned int i;

	float *result = (float *)MEM_mallocN(sizeof(float) * size, __func__);

	const float *fp_src = this->m_buffer;
	float       *fp_dst = result;

	for (i = 0; i < size; i++, fp_dst++, fp_src += COM_NUMBER_OF_CHANNELS) {
		*fp_dst = *fp_src;
	}

	return result;
}

float MemoryBuffer::getMaximumValue()
{
	float result = this->m_buffer[0];
	const unsigned int size = this->determineBufferSize();
	unsigned int i;

	const float *fp_src = this->m_buffer;

	for (i = 0; i < size; i++, fp_src += COM_NUMBER_OF_CHANNELS) {
		float value = *fp_src;
		if (value > result) {
			result = value;
		}
	}

	return result;
}

float MemoryBuffer::getMaximumValue(rcti *rect)
{
	rcti rect_clamp;

	/* first clamp the rect by the bounds or we get un-initialized values */
	BLI_rcti_isect(rect, &this->m_rect, &rect_clamp);

	if (!BLI_rcti_is_empty(&rect_clamp)) {
		MemoryBuffer *temp = new MemoryBuffer(NULL, &rect_clamp);
		temp->copyContentFrom(this);
		float result = temp->getMaximumValue();
		delete temp;
		return result;
	}
	else {
		BLI_assert(0);
		return 0.0f;
	}
}

MemoryBuffer::~MemoryBuffer()
{
	if (this->m_buffer) {
		MEM_freeN(this->m_buffer);
		this->m_buffer = NULL;
	}
}

void MemoryBuffer::copyContentFrom(MemoryBuffer *otherBuffer)
{
	if (!otherBuffer) {
		BLI_assert(0);
		return;
	}
	unsigned int otherY;
	unsigned int minX = max(this->m_rect.xmin, otherBuffer->m_rect.xmin);
	unsigned int maxX = min(this->m_rect.xmax, otherBuffer->m_rect.xmax);
	unsigned int minY = max(this->m_rect.ymin, otherBuffer->m_rect.ymin);
	unsigned int maxY = min(this->m_rect.ymax, otherBuffer->m_rect.ymax);
	int offset;
	int otherOffset;


	for (otherY = minY; otherY < maxY; otherY++) {
		otherOffset = ((otherY - otherBuffer->m_rect.ymin) * otherBuffer->m_chunkWidth + minX - otherBuffer->m_rect.xmin) * COM_NUMBER_OF_CHANNELS;
		offset = ((otherY - this->m_rect.ymin) * this->m_chunkWidth + minX - this->m_rect.xmin) * COM_NUMBER_OF_CHANNELS;
		memcpy(&this->m_buffer[offset], &otherBuffer->m_buffer[otherOffset], (maxX - minX) * COM_NUMBER_OF_CHANNELS * sizeof(float));
	}
}

void MemoryBuffer::writePixel(int x, int y, const float color[4])
{
	if (x >= this->m_rect.xmin && x < this->m_rect.xmax &&
	    y >= this->m_rect.ymin && y < this->m_rect.ymax)
	{
		const int offset = (this->m_chunkWidth * (y - this->m_rect.ymin) + x - this->m_rect.xmin) * COM_NUMBER_OF_CHANNELS;
		copy_v4_v4(&this->m_buffer[offset], color);
	}
}

void MemoryBuffer::addPixel(int x, int y, const float color[4])
{
	if (x >= this->m_rect.xmin && x < this->m_rect.xmax &&
	    y >= this->m_rect.ymin && y < this->m_rect.ymax)
	{
		const int offset = (this->m_chunkWidth * (y - this->m_rect.ymin) + x - this->m_rect.xmin) * COM_NUMBER_OF_CHANNELS;
		add_v4_v4(&this->m_buffer[offset], color);
	}
}


// table of (exp(ar) - exp(a)) / (1 - exp(a)) for r in range [0, 1] and a = -2
// used instead of actual gaussian, otherwise at high texture magnifications circular artifacts are visible
#define EWA_MAXIDX 255
static const float EWA_WTS[EWA_MAXIDX + 1] = {
	1.f, 0.990965f, 0.982f, 0.973105f, 0.96428f, 0.955524f, 0.946836f, 0.938216f, 0.929664f,
	0.921178f, 0.912759f, 0.904405f, 0.896117f, 0.887893f, 0.879734f, 0.871638f, 0.863605f,
	0.855636f, 0.847728f, 0.839883f, 0.832098f, 0.824375f, 0.816712f, 0.809108f, 0.801564f,
	0.794079f, 0.786653f, 0.779284f, 0.771974f, 0.76472f, 0.757523f, 0.750382f, 0.743297f,
	0.736267f, 0.729292f, 0.722372f, 0.715505f, 0.708693f, 0.701933f, 0.695227f, 0.688572f,
	0.68197f, 0.67542f, 0.66892f, 0.662471f, 0.656073f, 0.649725f, 0.643426f, 0.637176f,
	0.630976f, 0.624824f, 0.618719f, 0.612663f, 0.606654f, 0.600691f, 0.594776f, 0.588906f,
	0.583083f, 0.577305f, 0.571572f, 0.565883f, 0.56024f, 0.55464f, 0.549084f, 0.543572f,
	0.538102f, 0.532676f, 0.527291f, 0.521949f, 0.516649f, 0.511389f, 0.506171f, 0.500994f,
	0.495857f, 0.490761f, 0.485704f, 0.480687f, 0.475709f, 0.470769f, 0.465869f, 0.461006f,
	0.456182f, 0.451395f, 0.446646f, 0.441934f, 0.437258f, 0.432619f, 0.428017f, 0.42345f,
	0.418919f, 0.414424f, 0.409963f, 0.405538f, 0.401147f, 0.39679f, 0.392467f, 0.388178f,
	0.383923f, 0.379701f, 0.375511f, 0.371355f, 0.367231f, 0.363139f, 0.359079f, 0.355051f,
	0.351055f, 0.347089f, 0.343155f, 0.339251f, 0.335378f, 0.331535f, 0.327722f, 0.323939f,
	0.320186f, 0.316461f, 0.312766f, 0.3091f, 0.305462f, 0.301853f, 0.298272f, 0.294719f,
	0.291194f, 0.287696f, 0.284226f, 0.280782f, 0.277366f, 0.273976f, 0.270613f, 0.267276f,
	0.263965f, 0.26068f, 0.257421f, 0.254187f, 0.250979f, 0.247795f, 0.244636f, 0.241502f,
	0.238393f, 0.235308f, 0.232246f, 0.229209f, 0.226196f, 0.223206f, 0.220239f, 0.217296f,
	0.214375f, 0.211478f, 0.208603f, 0.20575f, 0.20292f, 0.200112f, 0.197326f, 0.194562f,
	0.191819f, 0.189097f, 0.186397f, 0.183718f, 0.18106f, 0.178423f, 0.175806f, 0.17321f,
	0.170634f, 0.168078f, 0.165542f, 0.163026f, 0.16053f, 0.158053f, 0.155595f, 0.153157f,
	0.150738f, 0.148337f, 0.145955f, 0.143592f, 0.141248f, 0.138921f, 0.136613f, 0.134323f,
	0.132051f, 0.129797f, 0.12756f, 0.125341f, 0.123139f, 0.120954f, 0.118786f, 0.116635f,
	0.114501f, 0.112384f, 0.110283f, 0.108199f, 0.106131f, 0.104079f, 0.102043f, 0.100023f,
	0.0980186f, 0.09603f, 0.094057f, 0.0920994f, 0.0901571f, 0.08823f, 0.0863179f, 0.0844208f,
	0.0825384f, 0.0806708f, 0.0788178f, 0.0769792f, 0.0751551f, 0.0733451f, 0.0715493f, 0.0697676f,
	0.0679997f, 0.0662457f, 0.0645054f, 0.0627786f, 0.0610654f, 0.0593655f, 0.0576789f, 0.0560055f,
	0.0543452f, 0.0526979f, 0.0510634f, 0.0494416f, 0.0478326f, 0.0462361f, 0.0446521f, 0.0430805f,
	0.0415211f, 0.039974f, 0.0384389f, 0.0369158f, 0.0354046f, 0.0339052f, 0.0324175f, 0.0309415f,
	0.029477f, 0.0280239f, 0.0265822f, 0.0251517f, 0.0237324f, 0.0223242f, 0.020927f, 0.0195408f,
	0.0181653f, 0.0168006f, 0.0154466f, 0.0141031f, 0.0127701f, 0.0114476f, 0.0101354f, 0.00883339f,
	0.00754159f, 0.00625989f, 0.00498819f, 0.00372644f, 0.00247454f, 0.00123242f, 0.f
};

static void radangle2imp(float a2, float b2, float th, float *A, float *B, float *C, float *F)
{
	float ct2 = cosf(th);
	const float st2 = 1.f - ct2 * ct2;    // <- sin(th)^2
	ct2 *= ct2;
	*A = a2 * st2 + b2 * ct2;
	*B = (b2 - a2) * sinf(2.f * th);
	*C = a2 * ct2 + b2 * st2;
	*F = a2 * b2;
}

// all tests here are done to make sure possible overflows are hopefully minimized
static void imp2radangle(float A, float B, float C, float F, float *a, float *b, float *th, float *ecc)
{
	if (F <= 1e-5f) {   // use arbitrary major radius, zero minor, infinite eccentricity
		*a = sqrtf(A > C ? A : C);
		*b = 0.f;
		*ecc = 1e10f;
		*th = 0.5f * (atan2f(B, A - C) + (float)M_PI);
	}
	else {
		const float AmC = A - C, ApC = A + C, F2 = F * 2.f;
		const float r = sqrtf(AmC * AmC + B * B);
		float d = ApC - r;
		*a = (d <= 0.f) ? sqrtf(A > C ? A : C) : sqrtf(F2 / d);
		d = ApC + r;
		if (d <= 0.f) {
			*b = 0.f;
			*ecc = 1e10f;
		}
		else {
			*b = sqrtf(F2 / d);
			*ecc = *a / *b;
		}
		/* incr theta by 0.5 * pi (angle of major axis) */
		*th = 0.5f * (atan2f(B, AmC) + (float)M_PI);
	}
}

static float clipuv(float x, float limit)
{
	x = (x < 0) ? 0 : ((x >= limit) ? (limit - 1) : x);
	return x;
}

/**
 * \note \a sampler at the moment is either 'COM_PS_NEAREST' or not, other values won't matter.
 */
void MemoryBuffer::readEWA(float result[4], float fx, float fy, float dx, float dy, PixelSampler sampler)
{
	const int width = this->getWidth(), height = this->getHeight();
	
	// scaling dxt/dyt by full resolution can cause overflow because of huge A/B/C and esp. F values,
	// scaling by aspect ratio alone does the opposite, so try something in between instead...
	const float ff2 = width, ff = sqrtf(ff2), q = height / ff;
	const float Ux = dx * ff, Vx = dx * q, Uy = dy * ff, Vy = dy * q;
	float A = Vx * Vx + Vy * Vy;
	float B = -2.f * (Ux * Vx + Uy * Vy);
	float C = Ux * Ux + Uy * Uy;
	float F = A * C - B * B * 0.25f;
	float a, b, th, ecc, a2, b2, ue, ve, U0, V0, DDQ, U, ac1, ac2, BU, d;
	int u, v, u1, u2, v1, v2;
	// The so-called 'high' quality ewa method simply adds a constant of 1 to both A & C,
	// so the ellipse always covers at least some texels. But since the filter is now always larger,
	// it also means that everywhere else it's also more blurry then ideally should be the case.
	// So instead here the ellipse radii are modified instead whenever either is too low.
	// Use a different radius based on interpolation switch, just enough to anti-alias when interpolation is off,
	// and slightly larger to make result a bit smoother than bilinear interpolation when interpolation is on
	// (minimum values: const float rmin = intpol ? 1.f : 0.5f;)

	/* note: 0.765625f is too sharp, 1.0 will not blur with an exact pixel sample
	 * useful to avoid blurring when there is no distortion */
#if 0
	const float rmin = ((sampler != COM_PS_NEAREST) ? 1.5625f : 0.765625f) / ff2;
#else
	const float rmin = ((sampler != COM_PS_NEAREST) ? 1.5625f : 1.0f     ) / ff2;
#endif
	imp2radangle(A, B, C, F, &a, &b, &th, &ecc);
	if ((b2 = b * b) < rmin) {
		if ((a2 = a * a) < rmin) {
			B = 0.f;
			A = C = rmin;
			F = A * C;
		}
		else {
			b2 = rmin;
			radangle2imp(a2, b2, th, &A, &B, &C, &F);
		}
	}

	ue = ff * sqrtf(C);
	ve = ff * sqrtf(A);
	d = (float)(EWA_MAXIDX + 1) / (F * ff2);
	A *= d;
	B *= d;
	C *= d;

	U0 = fx;
	V0 = fy;
	u1 = (int)(floorf(U0 - ue));
	u2 = (int)(ceilf(U0 + ue));
	v1 = (int)(floorf(V0 - ve));
	v2 = (int)(ceilf(V0 + ve));
	U0 -= 0.5f;
	V0 -= 0.5f;
	DDQ = 2.f * A;
	U = u1 - U0;
	ac1 = A * (2.f * U + 1.f);
	ac2 = A * U * U;
	BU = B * U;

	d = result[0] = result[1] = result[2] = result[3] = 0.f;
	for (v = v1; v <= v2; ++v) {
		const float V = v - V0;
		float DQ = ac1 + B * V;
		float Q = (C * V + BU) * V + ac2;
		for (u = u1; u <= u2; ++u) {
			if (Q < (float)(EWA_MAXIDX + 1)) {
				float tc[4];
				const float wt = EWA_WTS[(Q < 0.f) ? 0 : (unsigned int)Q];
				read(tc, clipuv(u, width), clipuv(v, height));
				madd_v4_v4fl(result, tc, wt);
				d += wt;
			}
			Q += DQ;
			DQ += DDQ;
		}
	}
	
	// d should hopefully never be zero anymore
	d = 1.f / d;
	mul_v4_fl(result, d);
}
