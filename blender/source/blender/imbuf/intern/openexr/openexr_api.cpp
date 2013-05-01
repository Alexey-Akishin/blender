/*
 * ***** BEGIN GPLLICENSE BLOCK *****
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
 * Copyright by Gernot Ziegler <gz@lysator.liu.se>.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Austin Benesh, Ton Roosendaal (float, half, speedup, cleanup...).
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/imbuf/intern/openexr/openexr_api.cpp
 *  \ingroup openexr
 */

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <fstream>
#include <string>
#include <set>
#include <errno.h>

#include <openexr_api.h>

#if defined (WIN32) && !defined(FREE_WINDOWS)
#include "utfconv.h"
#endif

extern "C"
{

// The following prevents a linking error in debug mode for MSVC using the libs in CVS
#if defined(WITH_OPENEXR) && defined(_WIN32) && defined(_DEBUG) && !defined(__MINGW32__) && !defined(__CYGWIN__)
_CRTIMP void __cdecl _invalid_parameter_noinfo(void)
{
}
#endif

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math_color.h"
#include "BLI_threads.h"

#include "IMB_imbuf_types.h"
#include "IMB_imbuf.h"
#include "IMB_allocimbuf.h"
#include "IMB_metadata.h"

#include "IMB_colormanagement.h"
#include "IMB_colormanagement_intern.h"

#include "openexr_multi.h"
}

#include <iostream>

#include <half.h>
#include <Iex.h>
#include <ImfVersion.h>
#include <ImathBox.h>
#include <ImfArray.h>
#include <ImfIO.h>
#include <ImfChannelList.h>
#include <ImfPixelType.h>
#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <ImfCompression.h>
#include <ImfCompressionAttribute.h>
#include <ImfStringAttribute.h>
#include <ImfStandardAttributes.h>
#include <ImfMultiView.h>
#include <ImfMultiPartInputFile.h>
#include <ImfInputPart.h>
#include <ImfOutputPart.h>
#include <ImfMultiPartOutputFile.h>
#include <ImfTiledOutputPart.h>
#include <ImfPartType.h>
#include <ImfPartHelper.h>

using namespace Imf;
using namespace Imath;

/* prototype */
static struct ExrView *imb_exr_get_view(ListBase *lb, char *viewname);

static struct ExrPass *imb_exr_get_pass(ListBase *lb, char *passname);

static int imb_exr_strip_view(const char *name, const char *viewname);

/* Memory Input Stream */

class Mem_IStream : public Imf::IStream
{
public:

	Mem_IStream (unsigned char *exrbuf, size_t exrsize) :
		IStream("dummy"), _exrpos(0), _exrsize(exrsize) {
		_exrbuf = exrbuf;
	}

	virtual bool    read(char c[], int n);
	virtual Int64   tellg();
	virtual void    seekg(Int64 pos);
	virtual void    clear();
	//virtual ~Mem_IStream() {}; // unused

private:

	Int64 _exrpos;
	Int64 _exrsize;
	unsigned char *_exrbuf;
};

bool Mem_IStream::read(char c[], int n)
{
	if (n + _exrpos <= _exrsize) {
		memcpy(c, (void *)(&_exrbuf[_exrpos]), n);
		_exrpos += n;
		return true;
	}
	else
		return false;
}

Int64 Mem_IStream::tellg()
{
	return _exrpos;
}

void Mem_IStream::seekg(Int64 pos)
{
	_exrpos = pos;
}

void Mem_IStream::clear()
{
}

/* File Input Stream */

class IFileStream : public Imf::IStream
{
public:
	IFileStream(const char *filename)
	: IStream(filename)
	{
		/* utf-8 file path support on windows */
#if defined (WIN32) && !defined(FREE_WINDOWS)
		wchar_t *wfilename = alloc_utf16_from_8(filename, 0);
		ifs.open(wfilename, std::ios_base::binary);
		free(wfilename);
#else
		ifs.open(filename, std::ios_base::binary);
#endif

		if (!ifs)
			Iex::throwErrnoExc();
	}

	virtual bool read(char c[], int n)
	{
		if (!ifs)
			throw Iex::InputExc("Unexpected end of file.");

		errno = 0;
		ifs.read(c, n);
		return check_error();
	}

	virtual Int64 tellg()
	{
		return std::streamoff(ifs.tellg());
	}

	virtual void seekg(Int64 pos)
	{
		ifs.seekg(pos);
		check_error();
	}

	virtual void clear()
	{
		ifs.clear();
	}

private:
	bool check_error()
	{
		if (!ifs) {
			if (errno)
				Iex::throwErrnoExc();

			return false;
		}

		return true;
	}

	std::ifstream ifs;
};

/* File Output Stream */

class OFileStream : public OStream
{
public:
	OFileStream(const char *filename)
	: OStream(filename)
	{
		/* utf-8 file path support on windows */
#if defined (WIN32) && !defined(FREE_WINDOWS)
		wchar_t *wfilename = alloc_utf16_from_8(filename, 0);
		ofs.open(wfilename, std::ios_base::binary);
		free(wfilename);
#else
		ofs.open(filename, std::ios_base::binary);
#endif

		if (!ofs)
			Iex::throwErrnoExc();
	}

	virtual void write(const char c[], int n)
	{
		errno = 0;
		ofs.write(c, n);
		check_error();
	}

	virtual Int64 tellp()
	{
		return std::streamoff(ofs.tellp());
	}

	virtual void seekp(Int64 pos)
	{
		ofs.seekp(pos);
		check_error();
	}

private:
	void check_error()
	{
		if (!ofs) {
			if (errno)
				Iex::throwErrnoExc();

			throw Iex::ErrnoExc("File output failed.");
		}
	}

	std::ofstream ofs;
};

struct _RGBAZ {
	half r;
	half g;
	half b;
	half a;
	half z;
};

typedef struct _RGBAZ RGBAZ;

extern "C"
{

int imb_is_a_openexr(unsigned char *mem)
{
	return Imf::isImfMagic((const char *)mem);
}

static void openexr_header_compression(Header *header, int compression)
{
	switch (compression) {
		case 0:
			header->compression() = NO_COMPRESSION;
			break;
		case 1:
			header->compression() = PXR24_COMPRESSION;
			break;
		case 2:
			header->compression() = ZIP_COMPRESSION;
			break;
		case 3:
			header->compression() = PIZ_COMPRESSION;
			break;
		case 4:
			header->compression() = RLE_COMPRESSION;
			break;
		default:
			header->compression() = ZIP_COMPRESSION;
			break;
	}
}

static void openexr_header_metadata(Header *header, struct ImBuf *ibuf)
{
	ImMetaData *info;

	for (info = ibuf->metadata; info; info = info->next)
		header->insert(info->key, StringAttribute(info->value));

	if (ibuf->ppm[0] > 0.0)
		addXDensity(*header, ibuf->ppm[0] / 39.3700787); /* 1 meter = 39.3700787 inches */
}

static int imb_save_openexr_half(struct ImBuf *ibuf, const char *name, int flags)
{
	const int channels = ibuf->channels;
	const int is_alpha = (channels >= 4) && (ibuf->planes == 32);
	const int is_zbuf = (flags & IB_zbuffloat) && ibuf->zbuf_float != NULL; /* summarize */
	const int width = ibuf->x;
	const int height = ibuf->y;

	try
	{
		Header header(width, height);

		openexr_header_compression(&header, ibuf->ftype & OPENEXR_COMPRESS);
		openexr_header_metadata(&header, ibuf);

		header.channels().insert("R", Channel(HALF));
		header.channels().insert("G", Channel(HALF));
		header.channels().insert("B", Channel(HALF));
		if (is_alpha)
			header.channels().insert("A", Channel(HALF));
		if (is_zbuf)     // z we do as float always
			header.channels().insert("Z", Channel(Imf::FLOAT));

		FrameBuffer frameBuffer;

		/* manually create ofstream, so we can handle utf-8 filepaths on windows */
		OFileStream file_stream(name);
		OutputFile file(file_stream, header);

		/* we store first everything in half array */
		RGBAZ *pixels = new RGBAZ[height * width];
		RGBAZ *to = pixels;
		int xstride = sizeof(RGBAZ);
		int ystride = xstride * width;

		/* indicate used buffers */
		frameBuffer.insert("R", Slice(HALF,  (char *) &pixels[0].r, xstride, ystride));
		frameBuffer.insert("G", Slice(HALF,  (char *) &pixels[0].g, xstride, ystride));
		frameBuffer.insert("B", Slice(HALF,  (char *) &pixels[0].b, xstride, ystride));
		if (is_alpha)
			frameBuffer.insert("A", Slice(HALF, (char *) &pixels[0].a, xstride, ystride));
		if (is_zbuf)
			frameBuffer.insert("Z", Slice(Imf::FLOAT, (char *)(ibuf->zbuf_float + (height - 1) * width),
			                              sizeof(float), sizeof(float) * -width));
		if (ibuf->rect_float) {
			float *from;

			for (int i = ibuf->y - 1; i >= 0; i--) {
				from = ibuf->rect_float + channels * i * width;

				for (int j = ibuf->x; j > 0; j--) {
					to->r = from[0];
					to->g = (channels >= 2) ? from[1] : from[0];
					to->b = (channels >= 3) ? from[2] : from[0];
					to->a = (channels >= 4) ? from[3] : 1.0f;
					to++; from += channels;
				}
			}
		}
		else {
			unsigned char *from;

			for (int i = ibuf->y - 1; i >= 0; i--) {
				from = (unsigned char *)ibuf->rect + 4 * i * width;

				for (int j = ibuf->x; j > 0; j--) {
					to->r = srgb_to_linearrgb((float)from[0] / 255.0f);
					to->g = srgb_to_linearrgb((float)from[1] / 255.0f);
					to->b = srgb_to_linearrgb((float)from[2] / 255.0f);
					to->a = channels >= 4 ? (float)from[3] / 255.0f : 1.0f;
					to++; from += 4;
				}
			}
		}

//		printf("OpenEXR-save: Writing OpenEXR file of height %d.\n", height);

		file.setFrameBuffer(frameBuffer);
		file.writePixels(height);

		delete[] pixels;
	}
	catch (const std::exception &exc)
	{
		printf("OpenEXR-save: ERROR: %s\n", exc.what());
		if (ibuf) IMB_freeImBuf(ibuf);

		return (0);
	}

	return (1);
}

static int imb_save_openexr_float(struct ImBuf *ibuf, const char *name, int flags)
{
	const int channels = ibuf->channels;
	const int is_alpha = (channels >= 4) && (ibuf->planes == 32);
	const int is_zbuf = (flags & IB_zbuffloat) && ibuf->zbuf_float != NULL; /* summarize */
	const int width = ibuf->x;
	const int height = ibuf->y;

	try
	{
		Header header(width, height);

		openexr_header_compression(&header, ibuf->ftype & OPENEXR_COMPRESS);
		openexr_header_metadata(&header, ibuf);

		header.channels().insert("R", Channel(Imf::FLOAT));
		header.channels().insert("G", Channel(Imf::FLOAT));
		header.channels().insert("B", Channel(Imf::FLOAT));
		if (is_alpha)
			header.channels().insert("A", Channel(Imf::FLOAT));
		if (is_zbuf)
			header.channels().insert("Z", Channel(Imf::FLOAT));

		FrameBuffer frameBuffer;

		/* manually create ofstream, so we can handle utf-8 filepaths on windows */
		OFileStream file_stream(name);
		OutputFile file(file_stream, header);

		int xstride = sizeof(float) * channels;
		int ystride = -xstride * width;
		float *rect[4] = {NULL, NULL, NULL, NULL};

		/* last scanline, stride negative */
		rect[0] = ibuf->rect_float + channels * (height - 1) * width;
		rect[1] = (channels >= 2) ? rect[0] + 1 : rect[0];
		rect[2] = (channels >= 3) ? rect[0] + 2 : rect[0];
		rect[3] = (channels >= 4) ? rect[0] + 3 : rect[0]; /* red as alpha, is this needed since alpha isn't written? */

		frameBuffer.insert("R", Slice(Imf::FLOAT,  (char *)rect[0], xstride, ystride));
		frameBuffer.insert("G", Slice(Imf::FLOAT,  (char *)rect[1], xstride, ystride));
		frameBuffer.insert("B", Slice(Imf::FLOAT,  (char *)rect[2], xstride, ystride));
		if (is_alpha)
			frameBuffer.insert("A", Slice(Imf::FLOAT,  (char *)rect[3], xstride, ystride));
		if (is_zbuf)
			frameBuffer.insert("Z", Slice(Imf::FLOAT, (char *) (ibuf->zbuf_float + (height - 1) * width),
			                              sizeof(float), sizeof(float) * -width));
		file.setFrameBuffer(frameBuffer);
		file.writePixels(height);
	}
	catch (const std::exception &exc)
	{
		printf("OpenEXR-save: ERROR: %s\n", exc.what());
		if (ibuf) IMB_freeImBuf(ibuf);

		return (0);
	}

	return (1);
	//	printf("OpenEXR-save: Done.\n");
}


int imb_save_openexr(struct ImBuf *ibuf, const char *name, int flags)
{
	if (flags & IB_mem) {
		printf("OpenEXR-save: Create EXR in memory CURRENTLY NOT SUPPORTED !\n");
		imb_addencodedbufferImBuf(ibuf);
		ibuf->encodedsize = 0;
		return(0);
	}

	if (ibuf->ftype & OPENEXR_HALF)
		return imb_save_openexr_half(ibuf, name, flags);
	else {
		/* when no float rect, we save as half (16 bits is sufficient) */
		if (ibuf->rect_float == NULL)
			return imb_save_openexr_half(ibuf, name, flags);
		else
			return imb_save_openexr_float(ibuf, name, flags);
	}
}

/* ********************* Nicer API, MultiLayer and with Tile file support ************************************ */

/* naming rules:
 * - parse name from right to left
 * - last character is channel ID, 1 char like 'A' 'R' 'G' 'B' 'X' 'Y' 'Z' 'W' 'U' 'V'
 * - separated with a dot; the Pass name (like "Depth", "Color", "Diffuse" or "Combined")
 * - separated with a dot: the Layer name (like "Lamp1" or "Walls" or "Characters")
 */

static ListBase exrhandles = {NULL, NULL};

typedef struct ExrHandle {
	struct ExrHandle *next, *prev;

	IFileStream *ifile_stream;
	MultiPartInputFile *ifile;

	OFileStream *ofile_stream;
	MultiPartOutputFile *mpofile;
	OutputFile *ofile;

	int tilex, tiley;
	int width, height;
	int mipmap;

	StringVector multiView;
	int parts;

	ListBase channels;  /* flattened out, ExrChannel */
	ListBase layers;    /* hierarchical, pointing in end to ExrChannel */
} ExrHandle;

/* flattened out channel */
typedef struct ExrChannel {
	struct ExrChannel *next, *prev;

	char name[EXR_TOT_MAXNAME + 1];           /* full name with everything */
	struct MultiViewChannelName *m;
	int xstride, ystride;                     /* step to next pixel, to next scanline */
	float *rect;                              /* first pointer to write in */
	char chan_id;                             /* quick lookup of channel char */
	int view_id;                              /* quick lookup of channel view */

} ExrChannel;


/* hierarchical; layers -> passes -> channels[] */
typedef struct ExrPass {
	struct ExrPass *next, *prev;
	char name[EXR_PASS_MAXNAME];
	int totchan;
	float *rect;
	struct ExrChannel *chan[EXR_PASS_MAXCHAN];
	char chan_id[EXR_PASS_MAXCHAN];
} ExrPass;

typedef struct ExrLayer {
	struct ExrLayer *next, *prev;
	char name[EXR_LAY_MAXNAME + 1];
	ListBase passes;
} ExrLayer;

/* ********************** */

void *IMB_exr_get_handle(void)
{
	ExrHandle *data = (ExrHandle *)MEM_callocN(sizeof(ExrHandle), "exr handle");
	BLI_addtail(&exrhandles, data);
	return data;
}

/* MultiView */
int IMB_exr_is_multiView(void *handle)
{
	ExrHandle *data = (ExrHandle *)handle;
	return data->multiView.size() > 1;
}

static StringVector IMB_exr_multiView(void *handle)
{
	ExrHandle *data = (ExrHandle *)handle;
	return data->multiView;
}

#if 0
void IMB_exr_write_multiViewHeader(Header *header, void *handle)
{
	addMultiView(*header, IMB_exr_multiView(handle));
}
#endif

void IMB_exr_add_view(void *handle, const char *name)
{
	ExrHandle *data = (ExrHandle *)handle;
	data->multiView.push_back(name);
}

int imb_exr_passInView(ExrPass *pass, const char *view)
{
	return (strstr(pass->name, view) != NULL);
}

int imb_exr_get_multiView_id(StringVector *views, const char *name)
{
	int count = 0;
	for (StringVector::const_iterator i = (*views).begin(); i != (*views).end(); ++i) {
		if (strcmp(name, &(*i)[0])==0)
			return count;
		else
			count ++;
	}
	return -1;
}

static StringVector imb_exr_get_views(MultiPartInputFile *file)
{
	StringVector views;
	if(file->parts() == 1) {
		if(hasMultiView(file->header(0))) {
			return multiView(file->header(0));
		}
	}

	for(int p=0;p<file->parts();p++) {
		std::string view="";
		if(file->header(p).hasView())
			view=file->header(p).view();

		if (imb_exr_get_multiView_id(&views, &view[0]) == -1)
			views.push_back(view);
		
	}

	return views;
}

#if 0
/* loose method, I tried using EXR viewFromChannelName() but it failed for some cases */
static const char *imb_exr_view_from_channel(const char *name, StringVector *views)
{
	static char view[EXR_VIEW_MAXNAME];
	char *a = NULL;
	int len;
	for (StringVector::const_iterator i = (*views).begin(); i != (*views).end(); ++i) {
		a = strstr(name, &(*i)[0]);
		if (a != NULL) {
			len = strlen(&(*i)[0]);
			break;
		}
	}

	if (a == NULL) return NULL;

	BLI_strncpy(view, a, len+1);
	return view;
}
#endif

#if 0
int imb_exr_viewIdFromChannelName(const char *name, StringVector *views)
{
	const char *view = imb_exr_view_from_channel(name, views);
	if (view == NULL) return 0;
	return imb_exr_get_multiView_id(views, view);
}
#endif

#if 0
const char *IMB_exr_insertViewName(void *handle, const char *pass, int view_id)
{
	StringVector views = IMB_exr_multiView(handle);
	static char name[EXR_PASS_MAXNAME + 1];
	int len;

	BLI_strncpy(name, pass, EXR_PASS_MAXNAME);
	len = strlen(name);
	name[len] = '.';
	BLI_strncpy(name+len, &views[view_id][0], EXR_PASS_MAXNAME-len);

	return name;
}
#endif

static const char *IMB_exr_insert_view_name(const char* passname, const char* view)
{
	if (view == NULL || view[0] == '\0')
		return passname;

	static char retstr[EXR_PASS_MAXNAME];
	const char *end = passname + strlen(passname);
	const char *token;

	int len = IMB_exr_split_token(passname, end, &token);

	if (len == 0)
		sprintf(retstr, "%s.%s", passname, view);
	else
		sprintf(retstr, "%.*s%s.%s", (int)(end-passname) - len, passname, view, token);

	return retstr;
}

/* from exrmultipart.cpp (OpenEXR source code) */
void imb_exr_make_unique_names (std::vector<Header> & headers)
{
	std::set<string> names;
	for ( size_t i = 0 ; i < headers.size() ; i++ ) {
		std::string base_name;
		// if no name at all, set it to <type><partnum> (first part is part 1)
		if (!headers[i].hasName()) {
			std::ostringstream s;
			s << headers[i].type() << (i + 1);
			base_name = s.str();
			headers[i].setName (base_name);
		}
		else {
			base_name = headers[i].name();
		}
		// check name has already been used, if so add a _<number> to it
		if (names.find (base_name) == names.end()) {
			std::ostringstream s;
			size_t backup=1;
			do {
				s.clear();
				s << headers[i].type() << i << "_" << backup;
				backup++;
			}
			while (names.find(s.str()) != names.end());
			headers[i].setName (s.str());
		}
	}
}

/* adds flattened ExrChannels */
/* xstride, ystride and rect can be done in set_channel too, for tile writing */
/* passname does not include view */
void IMB_exr_add_channel(void *handle, const char *layname, const char *passname, const char *view, int xstride, int ystride, float *rect)
{
	ExrHandle *data = (ExrHandle *)handle;
	ExrChannel *echan;

	echan = (ExrChannel *)MEM_callocN(sizeof(ExrChannel), "exr channel");
	echan->m = new MultiViewChannelName ();

	if (layname && layname[0] != '\0') {
		echan->m->name = layname;
		echan->m->name.append(".");
		echan->m->name.append(passname);
	}
	else {
		echan->m->name.assign(passname);
	}

	echan->m->view.assign(view ? view : "");

	/* name has to be unique, thus it's a combination of layer, pass, view, and channel */
	std::string raw_name = IMB_exr_insert_view_name(&(echan->m->name)[0], &(echan->m->view)[0]);
	BLI_strncpy(echan->name, &raw_name[0], EXR_TOT_MAXNAME - raw_name.size());

	echan->xstride = xstride;
	echan->ystride = ystride;
	echan->rect = rect;

	/* quick look up */
	echan->view_id = imb_exr_get_multiView_id(&data->multiView, &(echan->m->view)[0]);

	printf("added channel %s\n", echan->name);
	BLI_addtail(&data->channels, echan);
}

/* used for single part output files */
int IMB_exr_begin_write(void *handle, const char *filename, int width, int height, int compress)
{
	ExrHandle *data = (ExrHandle *)handle;
	Header header(width, height);
	ExrChannel *echan;
	int i;

	data->width = width;
	data->height = height;

	openexr_header_compression(&header, compress);
	// openexr_header_metadata(&header, ibuf); // no imbuf. cant write
	/* header.lineOrder() = DECREASING_Y; this crashes in windows for file read! */

	header.insert("BlenderMultiChannel", StringAttribute("Blender V2.55.1 and newer"));

	if (IMB_exr_is_multiView(handle))
		addMultiView(header, data->multiView);

	printf("\nIMB_exr_begin_write()\n");

	std::vector< MultiViewChannelName > channels;
	for (echan = (ExrChannel *)data->channels.first; echan; echan = echan->next) {
		header.channels().insert(echan->name, Channel(Imf::FLOAT));
		channels.push_back(*echan->m);
	}

	/* this set the part number to be used in IMB_exr_write_channels() */
	data->parts = SplitChannels(channels.begin(), channels.end(), false, (data->multiView.size()>0?data->multiView[0]:""));

	for (i=0, echan = (ExrChannel *)data->channels.first; echan; echan = echan->next, i++) {
		echan->m->name = channels[i].name;
		echan->m->view = channels[i].view;
		echan->m->part_number = channels[i].part_number;
		echan->m->internal_name = channels[i].internal_name;
	}

	/* avoid crash/abort when we don't have permission to write here */
	/* manually create ofstream, so we can handle utf-8 filepaths on windows */
	try {
		data->ofile_stream = new OFileStream(filename);
		data->ofile = new OutputFile(*(data->ofile_stream), header);
	}
	catch (const std::exception &exc) {
		std::cerr << "IMB_exr_begin_write: ERROR: " << exc.what() << std::endl;

		delete data->ofile;
		delete data->ofile_stream;

		data->ofile = NULL;
		data->ofile_stream = NULL;
	}

	return (data->ofile != NULL);
}

/* only used for writing temp. render results (not image files)
 * (FSA and Save Buffers) */
void IMB_exrtile_begin_write(void *handle, const char *filename, int mipmap, int width, int height, int tilex, int tiley)
{
	ExrHandle *data = (ExrHandle *)handle;
	Header header(width, height);
	std::vector<Header> headers;
	ExrChannel *echan;

	data->tilex = tilex;
	data->tiley = tiley;
	data->width = width;
	data->height = height;
	data->mipmap = mipmap;

	header.setTileDescription(TileDescription(tilex, tiley, (mipmap) ? MIPMAP_LEVELS : ONE_LEVEL));
	header.compression() = RLE_COMPRESSION;
	header.setType(TILEDIMAGE);

	header.insert("BlenderMultiChannel", StringAttribute("Blender V2.66"));

	int numparts = data->multiView.size();

	/* copy header from all parts of input to our header array
	 * those temporary files have one part per view */
	for (int i = 0; i < numparts; i++)
	{
		headers.push_back (header);
		headers[headers.size()-1].setView(data->multiView[i]);
		headers[headers.size()-1].setName(data->multiView[i]);
	}

	printf("\nIMB_exrtile_begin_write\n");
	printf("%s %-6s %-22s \"%s\"\n", "p", "view", "name", "internal_name");
	printf("---------------------------------------------------------------\n");

	/* assign channels  */
	for (echan = (ExrChannel *)data->channels.first; echan; echan = echan->next) {

		echan->m->internal_name = echan->m->name;
		echan->m->part_number = echan->view_id;

		headers[echan->view_id].channels().insert(echan->m->internal_name, Channel(Imf::FLOAT));
		printf("%d %-6s %-22s \"%s\"\n", echan->m->part_number, &(echan->m->view)[0], &(echan->m->name)[0], &(echan->m->internal_name)[0]);
	}

	/* avoid crash/abort when we don't have permission to write here */
	/* manually create ofstream, so we can handle utf-8 filepaths on windows */
	try {
		data->ofile_stream = new OFileStream(filename);
		data->mpofile = new MultiPartOutputFile(*(data->ofile_stream), &headers[0], headers.size());
	}
	catch (const std::exception &exc) {
		std::cerr << exc.what() << std::endl;

		delete data->mpofile;
		delete data->ofile_stream;

		data->mpofile = NULL;
		data->ofile_stream = NULL;
	}
}

/* used for final output images */
int IMB_exrmultipart_begin_write(void *handle, const char *filename, int width, int height, int compress)
{
	ExrHandle *data = (ExrHandle *)handle;
	Header header(width, height);
	std::vector<Header> headers;
	ExrChannel *echan;
	int i;

	data->width = width;
	data->height = height;

	openexr_header_compression(&header, compress);
	header.insert("BlenderMultiPart", StringAttribute("Blender V2.66"));
	header.setType(SCANLINEIMAGE);

	printf("\nIMB_exrmultipart_begin_write()\n");

	std::vector< MultiViewChannelName > channels;
	for (echan = (ExrChannel *)data->channels.first; echan; echan = echan->next)
		channels.push_back(*echan->m);

	/* this set the part number to be used in IMB_exrmultipart_write_channels() */
	data->parts = SplitChannels(channels.begin(), channels.end());

	for (i=0, echan = (ExrChannel *)data->channels.first; echan; echan = echan->next, i++) {
		echan->m->name = channels[i].name;
		echan->m->view = channels[i].view;
		echan->m->part_number = channels[i].part_number;
		echan->m->internal_name = channels[i].internal_name;
	}

	printf("channel[%s]: %s %-6s %-22s \"%s\"\n", "id", "p", "view", "name", "internal_name");
	printf("---------------------------------------------------------------------\n");

	for (size_t i=0; i < channels.size(); i++) {
		printf("channel[%2d]: %d %-6s %-22s \"%s\"\n", int(i), channels[i].part_number, &(channels[i].view)[0], &(channels[i].name)[0], &(channels[i].internal_name)[0]);
	}
	
	/* initialize the header */
	for (int i = 0; i < data->parts; i++)	{
		headers.push_back (header);
		headers[0].channels() = ChannelList();
	}

	/* assign channels  */
	for (size_t i=0; i < channels.size(); i++) {
		headers[channels[i].part_number].channels().insert(channels[i].internal_name, Channel(Imf::FLOAT));
		if (channels[i].view != "")
			headers[channels[i].part_number].setView(channels[i].view);
	}

	imb_exr_make_unique_names(headers);

	/* avoid crash/abort when we don't have permission to write here */
	/* manually create ofstream, so we can handle utf-8 filepaths on windows */
	try {
		data->ofile_stream = new OFileStream(filename);
		data->mpofile = new MultiPartOutputFile(*(data->ofile_stream), &headers[0], headers.size());
		printf("exr: success creating file: \"%s\"\n", filename);
	}
	catch (const std::exception &exc) {
		std::cerr << exc.what() << std::endl;

		delete data->mpofile;
		delete data->ofile_stream;

		data->mpofile = NULL;
		data->ofile_stream = NULL;
	}
	return (data->mpofile != NULL);
}

/* read from file */
int IMB_exr_begin_read(void *handle, const char *filename, int *width, int *height)
{
	ExrHandle *data = (ExrHandle *)handle;
	ExrChannel *echan;

	if (BLI_exists(filename) && BLI_file_size(filename) > 32) {   /* 32 is arbitrary, but zero length files crashes exr */
		/* avoid crash/abort when we don't have permission to write here */
		try {
			data->ifile_stream = new IFileStream(filename);
			data->ifile = new MultiPartInputFile(*(data->ifile_stream));
		}
		catch (const std::exception &exc) {
			delete data->ifile;
			delete data->ifile_stream;

			data->ifile = NULL;
			data->ifile_stream = NULL;
		}

		if (data->ifile) {
			Box2i dw = data->ifile->header(0).dataWindow();
			data->width = *width  = dw.max.x - dw.min.x + 1;
			data->height = *height = dw.max.y - dw.min.y + 1;

			data->multiView = imb_exr_get_views(data->ifile);

			std::vector<MultiViewChannelName> channels;
			GetChannelsInMultiPartFile(*data->ifile, channels);

			for(size_t i = 0; i < channels.size(); i++) {
				IMB_exr_add_channel(data, NULL, &(channels[i].name)[0], &(channels[i].view)[0], 0, 0, NULL);
		
				echan = (ExrChannel *)data->channels.last;
				echan->m->name = channels[i].name;
				echan->m->view = channels[i].view;
				echan->m->part_number = channels[i].part_number;
				echan->m->internal_name = channels[i].internal_name;
			}

			return 1;
		}
	}
	return 0;
}

/* still clumsy name handling, layers/channels can be ordered as list in list later */
/* passname here is the raw channel name without the layer */
void IMB_exr_set_channel(void *handle, const char *layname, const char *passname, int xstride, int ystride, float *rect)
{
	ExrHandle *data = (ExrHandle *)handle;
	ExrChannel *echan;
	char name[EXR_TOT_MAXNAME + 1];

	if (layname) {
		char lay[EXR_LAY_MAXNAME + 1], pass[EXR_PASS_MAXNAME + 1];
		BLI_strncpy(lay, layname, EXR_LAY_MAXNAME);
		BLI_strncpy(pass, passname, EXR_PASS_MAXNAME);

		BLI_snprintf(name, sizeof(name), "%s.%s", lay, pass);
	}
	else
		BLI_strncpy(name, passname, EXR_TOT_MAXNAME - 1);

	echan = (ExrChannel *)BLI_findstring(&data->channels, name, offsetof(ExrChannel, name));

	if (echan) {
		echan->xstride = xstride;
		echan->ystride = ystride;
		echan->rect = rect;
	}
	else
		printf("IMB_exr_set_channel error %s\n", name);
}

void IMB_exr_clear_channels(void *handle)
{
	ExrHandle *data = (ExrHandle *)handle;
	ExrChannel *chan;

	for (chan = (ExrChannel *)data->channels.first; chan; chan = chan->next)
		delete chan->m;

	BLI_freelistN(&data->channels);
}

void IMB_exr_write_channels(void *handle)
{
	ExrHandle *data = (ExrHandle *)handle;
	FrameBuffer frameBuffer;
	ExrChannel *echan;

	if (data->channels.first) {
		for (echan = (ExrChannel *)data->channels.first; echan; echan = echan->next) {
			/* last scanline, stride negative */
			float *rect = echan->rect + echan->xstride * (data->height - 1) * data->width;
			frameBuffer.insert(echan->m->internal_name,
							   Slice(Imf::FLOAT,
									 (char *)rect,
									 echan->xstride * sizeof(float),
									 -echan->ystride * sizeof(float)
									 )
							   );
		}

		data->ofile->setFrameBuffer(frameBuffer);
		try {
			data->ofile->writePixels(data->height);
		}
		catch (const std::exception &exc) {
			std::cerr << "OpenEXR-writePixels: ERROR: " << exc.what() << std::endl;
		}
	}
	else {
		printf("Error: attempt to save MultiLayer without layers.\n");
	}
}

/* temporary function, used for FSA and Save Buffers */
/* called once per tile * view */
void IMB_exrtile_write_channels(void *handle, int partx, int party, int level, int view)
{
	ExrHandle *data = (ExrHandle *)handle;
	FrameBuffer frameBuffer;
	ExrChannel *echan;

	printf("\nIMB_exrtile_write_channels(view: %d)\n", view);
	printf("%s %-6s %-22s \"%s\"\n", "p", "view", "name", "internal_name");
	printf("---------------------------------------------------------------------\n");

	for (echan = (ExrChannel *)data->channels.first; echan; echan = echan->next) {

		//MV CLEANUP: this may not be needed
		if (view != echan->view_id)
			continue;

		printf("%d %-6s %-22s \"%s\"\n",
				echan->m->part_number,
				&(echan->m->view)[0],
				&(echan->m->name)[0],
				&(echan->m->internal_name)[0]
			   );

		float *rect = echan->rect - echan->xstride * partx - echan->ystride * party;
		frameBuffer.insert(echan->m->internal_name, Slice(Imf::FLOAT,  (char *)rect,
		                                      echan->xstride * sizeof(float), echan->ystride * sizeof(float)));
	}

	TiledOutputPart out (*data->mpofile, view);
	out.setFrameBuffer(frameBuffer);

	try {
		// printf("write tile %d %d\n", partx/data->tilex, party/data->tiley);
		out.writeTile(partx / data->tilex, party / data->tiley, level);
	}
	catch (const std::exception &exc) {
		std::cerr << "OpenEXR-writeTile: ERROR: " << exc.what() << std::endl;
	}
	printf("\n");
}

/* called only when handle has all views */
void IMB_exrmultipart_write_channels(void *handle)
{
	ExrHandle *data = (ExrHandle *)handle;
	std::vector <FrameBuffer> frameBuffers(data->parts);
	std::vector <OutputPart> outputParts;
	ExrChannel *echan;
	int i;

	if (data->channels.first == NULL)
		return;

	printf("\nIMB_exrmultipart_write_channels()\n");

	for (echan = (ExrChannel *)data->channels.first; echan; echan = echan->next) {
		/* last scanline, stride negative */
		float *rect = echan->rect + echan->xstride * (data->height - 1) * data->width;
		frameBuffers[echan->m->part_number].insert(echan->m->internal_name,
												   Slice(Imf::FLOAT,
														 (char *)rect,
														 echan->xstride * sizeof(float),
														 -echan->ystride * sizeof(float)
														 )
												   );
	}

	for (i=0; i < data->parts; i++) {
		OutputPart out(*data->mpofile, i);
		out.setFrameBuffer(frameBuffers[i]);
		outputParts.push_back(out);
	}

	try {
		for (i=0; i < data->parts; i++)
			outputParts[i].writePixels(data->height);
	}
	catch (const std::exception &exc) {
		std::cerr << "OpenEXR-write Multi Part: ERROR: " << exc.what() << std::endl;
	}
}

void IMB_exr_multilayer_convert(void *handle, void *base,
								void * (*addview)(void *base, const char *str),
								void * (*addlayer)(void *base, const char *str),
								void (*addpass)(void *base, void *lay, const char *str,
												float *rect, int totchan,
												const char *chan_id, int view_id))
{
	ExrHandle *data = (ExrHandle *)handle;
	ExrLayer *lay;
	ExrPass *pass;
	int is_multiview;
	StringVector views;
	std::string viewname = "";
	int view_id = 0;

	/* add views to RenderResult */
	for (StringVector::const_iterator i = data->multiView.begin(); i != data->multiView.end(); ++i) {
		addview(base, &(*i)[0]);
	}

	if (data->layers.first == NULL) {
		printf("cannot convert multilayer, no layers in handle\n");
		return;
	}

	is_multiview = hasMultiView(data->ifile->header(0));
	if (is_multiview)
		views = IMB_exr_multiView(handle);

	for (lay = (ExrLayer *)data->layers.first; lay; lay = lay->next) {
		void *laybase = addlayer(base, lay->name);
		if (laybase) {
			for (pass = (ExrPass *)lay->passes.first; pass; pass = pass->next) {
				addpass(base, laybase, pass->name, pass->rect, pass->totchan, pass->chan_id, view_id);
				pass->rect = NULL;
			}
		}
	}
}

void IMB_exr_read_channels(void *handle)
{
	ExrHandle *data = (ExrHandle *)handle;
	ExrChannel *echan;
	int numparts = data->ifile->parts();
	std::vector<FrameBuffer> frameBuffers(numparts);
	std::vector<InputPart> inputParts;

	/* check if exr was saved with previous versions of blender which flipped images */
	const StringAttribute *ta = data->ifile->header(0).findTypedAttribute <StringAttribute> ("BlenderMultiChannel");
	short flip = (ta && strncmp(ta->value().c_str(), "Blender V2.43", 13) == 0); /* 'previous multilayer attribute, flipped */

	printf("\nIMB_exr_read_channels\n");
	printf("%s %-6s %-22s \"%s\"\n", "p", "view", "name", "internal_name");
	printf("---------------------------------------------------------------------\n");

	for (echan = (ExrChannel *)data->channels.first; echan; echan = echan->next) {
		printf("%d %-6s %-22s \"%s\"\n",
				echan->m->part_number,
				&(echan->m->view)[0],
				&(echan->m->name)[0],
				&(echan->m->internal_name)[0]
			   );

		if (echan->rect) {
			if (flip)
				frameBuffers[echan->m->part_number].insert(echan->m->internal_name,
														   Slice(Imf::FLOAT,
																 (char *)echan->rect,
																 echan->xstride * sizeof(float),
																 echan->ystride * sizeof(float)
																 )
														   );
			else
				frameBuffers[echan->m->part_number].insert(echan->m->internal_name,
														   Slice(Imf::FLOAT,
																 (char *)(echan->rect + echan->xstride * (data->height - 1) * data->width),
																 echan->xstride * sizeof(float),
																 -echan->ystride * sizeof(float)
																 )
														   );
		}
		else
			printf("warning, channel with no rect set %s\n", &(echan->m->internal_name)[0]);
	}

	for (int i = 0; i < numparts; i++) {
		InputPart in (*data->ifile, i);
		in.setFrameBuffer(frameBuffers[i]);
		inputParts.push_back(in);
	}

	try {
		for (int i = 0; i < numparts; i++) {
			Header header = inputParts[i].header();
			printf("readPixels:readPixels[%d]: min.y: %d, max.y: %d\n", i, header.dataWindow().min.y, header.dataWindow().max.y);
			inputParts[i].readPixels(header.dataWindow().min.y, header.dataWindow().max.y);
			inputParts[i].readPixels(0, data->height - 1);
		}
	}
	catch (const std::exception &exc) {
		std::cerr << "OpenEXR-readPixels: ERROR: " << exc.what() << std::endl;
	}
}

void IMB_exr_close(void *handle)
{
	ExrHandle *data = (ExrHandle *)handle;
	ExrLayer *lay;
	ExrPass *pass;
	ExrChannel *chan;

	delete data->ifile;
	delete data->ifile_stream;
	delete data->ofile;
	delete data->mpofile;
	delete data->ofile_stream;

	data->ifile = NULL;
	data->ifile_stream = NULL;
	data->ofile = NULL;
	data->mpofile = NULL;
	data->ofile_stream = NULL;

	for (chan = (ExrChannel *)data->channels.first; chan; chan = chan->next) {
		delete chan->m;
	}
	BLI_freelistN(&data->channels);

	for (lay = (ExrLayer *)data->layers.first; lay; lay = lay->next) {
		for (pass = (ExrPass *)lay->passes.first; pass; pass = pass->next)
			if (pass->rect)
				MEM_freeN(pass->rect);
		BLI_freelistN(&lay->passes);
	}
	BLI_freelistN(&data->layers);

	BLI_remlink(&exrhandles, data);
	MEM_freeN(data);
}

/* ********* */

/* get a substring from the end of the name, separated by '.' */
int IMB_exr_split_token(const char *str, const char *end, const char **token)
{
	ptrdiff_t maxlen = end - str;
	int len = 0;
	while (len < maxlen && *(end - len - 1) != '.') {
		len++;
	}

	*token = end - len;
	return len;
}

static int imb_exr_strip_view(const char *name, const char *viewname)
{
	/**
	 A -> A
	 left.R -> R
	 main.right.depth -> main.depth
	 main.depth.left.z -> main.depth.z */
	if (viewname == NULL)
		return 0;

	const char *end = name + strlen(name);
	char *a = strstr(name, viewname);
	char *b = a + BLI_strlen_utf8(viewname) + 1; /* +1 to skip '.' separator */

	if ((a == NULL) || (b == NULL) || (b < a) || (b > end))
		return 0;

	memmove(a, b, strlen(b) + 1);
	return (int)(a - name);
}

static int imb_exr_split_channel_name(ExrChannel *echan, char *layname, char *passname)
{
	const char *name = &(echan->m->name)[0];
	const char *end = name + strlen(name);
	const char *token;
	char tokenbuf[EXR_TOT_MAXNAME];
	int len;

	/* some multilayers have the combined buffer with names A B G R saved */
	if (name[1] == 0) {
		echan->chan_id = name[0];
		layname[0] = '\0';
		strcpy(passname, "Combined");
		return 1;
	}

	/* last token is single character channel identifier */
	len = IMB_exr_split_token(name, end, &token);
	if (len == 0) {
		printf("multilayer read: bad channel name: %s\n", name);
		return 0;
	}
	else if (len > 1) {
		BLI_strncpy(tokenbuf, token, len);
		printf("multilayer read: channel token too long: %s\n", tokenbuf);
		return 0;
	}
	echan->chan_id = token[0];
	end -= len + 1; /* +1 to skip '.' separator */

	/* second token is pass name */
	len = IMB_exr_split_token(name, end, &token);
	if (len == 0) {
		printf("multilayer read: bad channel name: %s\n", name);
		return 0;
	}

	BLI_strncpy(passname, token, len + 1);
	end -= len + 1; /* +1 to skip '.' separator */

	/* all preceding tokens combined as layer name */
	if (end > name)
		BLI_strncpy(layname, name, (int)(end - name) + 1);
	else
		layname[0] = '\0';

	return 1;
}

static ExrLayer *imb_exr_get_layer(ListBase *lb, char *layname)
{
	ExrLayer *lay = (ExrLayer *)BLI_findstring(lb, layname, offsetof(ExrLayer, name));

	if (lay == NULL) {
		lay = (ExrLayer *)MEM_callocN(sizeof(ExrLayer), "exr layer");
		BLI_addtail(lb, lay);
		BLI_strncpy(lay->name, layname, EXR_LAY_MAXNAME);
	}

	return lay;
}

static ExrPass *imb_exr_get_pass(ListBase *lb, char *passname)
{
	ExrPass *pass = (ExrPass *)BLI_findstring(lb, passname, offsetof(ExrPass, name));

	if (pass == NULL) {
		pass = (ExrPass *)MEM_callocN(sizeof(ExrPass), "exr pass");

		/* if passname starts with "Combined" */
		if (strstr(passname, "Combined") == passname)
			BLI_addhead(lb, pass);
		else
			BLI_addtail(lb, pass);
	}

	BLI_strncpy(pass->name, passname, EXR_LAY_MAXNAME);

	return pass;
}

/* creates channels, makes a hierarchy and assigns memory to channels */
static ExrHandle *imb_exr_begin_read_mem(MultiPartInputFile *file, int width, int height)
{
	ExrLayer *lay;
	ExrPass *pass;
	ExrChannel *echan;
	ExrHandle *data = (ExrHandle *)IMB_exr_get_handle();
	int a;
	char layname[EXR_TOT_MAXNAME], passname[EXR_TOT_MAXNAME];

	data->ifile = file;
	data->width = width;
	data->height = height;

	std::vector<MultiViewChannelName> channels;
	GetChannelsInMultiPartFile(*data->ifile, channels);

	data->multiView = imb_exr_get_views(data->ifile);

	for(size_t i = 0; i < channels.size(); i++) {
		IMB_exr_add_channel(data, NULL, &(channels[i].name)[0], &(channels[i].view)[0], 0, 0, NULL);

		echan = (ExrChannel *)data->channels.last;
		echan->m->name = channels[i].name;
		echan->m->view = channels[i].view;
		echan->m->part_number = channels[i].part_number;
		echan->m->internal_name = channels[i].internal_name;
	}

	/* now try to sort out how to assign memory to the channels */
	/* first build hierarchical layer list */
	for (echan = (ExrChannel *)data->channels.first; echan; echan = echan->next) {
		if (imb_exr_split_channel_name(echan, layname, passname)) {

			/* UV/Image Editor can't handle a pass with two images
			 * for now using view in the pass name //MV */
			if (echan->m->view[0] != '\0')
				sprintf(passname, "%s.%s", passname, &(echan->m->view)[0]);

			ExrLayer *lay = imb_exr_get_layer(&data->layers, layname);
			ExrPass *pass = imb_exr_get_pass(&lay->passes, passname);

			pass->chan[pass->totchan] = echan;
			pass->totchan++;
			if (pass->totchan >= EXR_PASS_MAXCHAN)
				break;
		}

	}
	if (echan) {
		printf("error, too many channels in one pass: %s\n", &(echan->m->name)[0]);
		IMB_exr_close(data);
		return NULL;
	}

	/* with some heuristics, try to merge the channels in buffers */
	for (lay = (ExrLayer *)data->layers.first; lay; lay = lay->next) {
		for (pass = (ExrPass *)lay->passes.first; pass; pass = pass->next) {
			if (pass->totchan) {
				pass->rect = (float *)MEM_mapallocN(width * height * pass->totchan * sizeof(float), "pass rect");
				if (pass->totchan == 1) {
					echan = pass->chan[0];
					echan->rect = pass->rect;
					echan->xstride = 1;
					echan->ystride = width;
					pass->chan_id[0] = echan->chan_id;
				}
				else {
					char lookup[256];

					memset(lookup, 0, sizeof(lookup));

					/* we can have RGB(A), XYZ(W), UVA */
					if ((pass->totchan == 3) || (pass->totchan == 4)) {
						if (pass->chan[0]->chan_id == 'B' || pass->chan[1]->chan_id == 'B' ||  pass->chan[2]->chan_id == 'B') {
							lookup[(unsigned int)'R'] = 0;
							lookup[(unsigned int)'G'] = 1;
							lookup[(unsigned int)'B'] = 2;
							lookup[(unsigned int)'A'] = 3;
						}
						else if (pass->chan[0]->chan_id == 'Y' || pass->chan[1]->chan_id == 'Y' ||  pass->chan[2]->chan_id == 'Y') {
							lookup[(unsigned int)'X'] = 0;
							lookup[(unsigned int)'Y'] = 1;
							lookup[(unsigned int)'Z'] = 2;
							lookup[(unsigned int)'W'] = 3;
						}
						else {
							lookup[(unsigned int)'U'] = 0;
							lookup[(unsigned int)'V'] = 1;
							lookup[(unsigned int)'A'] = 2;
						}
						for (a = 0; a < pass->totchan; a++) {
							echan = pass->chan[a];
							echan->rect = pass->rect + lookup[(unsigned int)echan->chan_id] + echan->view_id * (pass->totchan % 3 == 0?3:4);
							echan->xstride = pass->totchan;
							echan->ystride = width * pass->totchan;
							pass->chan_id[(unsigned int)lookup[(unsigned int)echan->chan_id]] = echan->chan_id;
						}
					}
					else { /* unknown */
						for (a = 0; a < pass->totchan; a++) {
							echan = pass->chan[a];
							echan->rect = pass->rect + a;
							echan->xstride = pass->totchan;
							echan->ystride = width * pass->totchan;
							pass->chan_id[a] = echan->chan_id;
						}
					}
				}
			}
		}
	}

	return data;
}


/* ********************************************************* */

/* debug only */
static void exr_print_filecontents(MultiPartInputFile *file)
{
	int numparts = file->parts();
	if (numparts == 1 && hasMultiView(file->header(0))) {
		const StringVector views = multiView(file->header(0));
		printf("OpenEXR-load: MultiView file\n");
		printf("OpenEXR-load: Default view: %s\n", &defaultViewName(views)[0]);
		for (StringVector::const_iterator i = views.begin(); i != views.end(); ++i) {
			printf("OpenEXR-load: Found view %s\n", &(*i)[0]);
		}
	}
	else if (numparts > 1) {
		printf("OpenEXR-load: MultiPart file\n");
		for (int i = 0; i < numparts; i++) {
			if (file->header(i).hasView())
				printf("OpenEXR-load: Part %d: view = \"%s\"\n", i, &(file->header(i).view())[0]);
		}
	}

	for (int j = 0; j < numparts; j++) {
		const ChannelList &channels = file->header(j).channels();
		for (ChannelList::ConstIterator i = channels.begin(); i != channels.end(); ++i) {
			const Channel &channel = i.channel();
			printf("OpenEXR-load: Found channel %s of type %d\n", i.name(), channel.type);
		}
	}
}

/* for non-multilayer, map  R G B A channel names to something that's in this file */
static const char *exr_rgba_channelname(MultiPartInputFile *file, const char *chan)
{
	const ChannelList &channels = file->header(0).channels();

	for (ChannelList::ConstIterator i = channels.begin(); i != channels.end(); ++i) {
		/* const Channel &channel = i.channel(); */ /* Not used yet */
		const char *str = i.name();
		int len = strlen(str);
		if (len) {
			if (BLI_strcasecmp(chan, str + len - 1) == 0) {
				return str;
			}
		}
	}
	return chan;
}

static int exr_has_zbuffer(MultiPartInputFile *file)
{
	return !(file->header(0).channels().findChannel("Z") == NULL);
}

static int exr_has_alpha(MultiPartInputFile *file)
{
	return !(file->header(0).channels().findChannel("A") == NULL);
}

static int exr_is_multilayer(MultiPartInputFile *file)
{
	const StringAttribute *comments = file->header(0).findTypedAttribute<StringAttribute>("BlenderMultiChannel");
	const ChannelList &channels = file->header(0).channels();
	std::set <std::string> layerNames;

	/* will not include empty layer names */
	channels.layers(layerNames);

	if (comments || layerNames.size() > 1)
		return 1;

	if (layerNames.size()) {
		/* if layerNames is not empty, it means at least one layer is non-empty,
		 * but it also could be layers without names in the file and such case
		 * shall be considered a multilayer exr
		 *
		 * that's what we do here: test whether there're empty layer names together
		 * with non-empty ones in the file
		 */
		for (ChannelList::ConstIterator i = channels.begin(); i != channels.end(); i++) {
			std::string layerName = i.name();
			size_t pos = layerName.rfind ('.');

			if (pos == std::string::npos)
				return 1;
		}
	}

	return 0;
}

struct ImBuf *imb_load_openexr(unsigned char *mem, size_t size, int flags, char colorspace[IM_MAX_SPACE])
{
	struct ImBuf *ibuf = NULL;
	MultiPartInputFile *file = NULL;

	if (imb_is_a_openexr(mem) == 0) return(NULL);

	colorspace_set_default_role(colorspace, IM_MAX_SPACE, COLOR_ROLE_DEFAULT_FLOAT);

	try
	{
		Mem_IStream *membuf = new Mem_IStream(mem, size);
		int is_multilayer, is_multipart;

		file = new MultiPartInputFile(*membuf);

		Box2i dw = file->header(0).dataWindow();
		const int width  = dw.max.x - dw.min.x + 1;
		const int height = dw.max.y - dw.min.y + 1;

		//printf("OpenEXR-load: image data window %d %d %d %d\n",
		//	   dw.min.x, dw.min.y, dw.max.x, dw.max.y);

		if (1) // debug
			exr_print_filecontents(file);

		is_multilayer = exr_is_multilayer(file);
		is_multipart = file->parts();

		/* do not make an ibuf when */
		if (is_multilayer && !(flags & IB_test) && !(flags & IB_multilayer)) {
			printf("Error: can't process EXR multilayer file\n");
		}
		else {
			const int is_alpha = exr_has_alpha(file);

			ibuf = IMB_allocImBuf(width, height, is_alpha ? 32 : 24, 0);

			if (hasXDensity(file->header(0))) {
				ibuf->ppm[0] = xDensity(file->header(0)) * 39.3700787f;
				ibuf->ppm[1] = ibuf->ppm[0] * (double)file->header(0).pixelAspectRatio();
			}

			ibuf->ftype = OPENEXR;

			if (!(flags & IB_test)) {
				if (is_multilayer || is_multipart) { /* only enters with IB_multilayer flag set */
					/* constructs channels for reading, allocates memory in channels */
					ExrHandle *handle = imb_exr_begin_read_mem(file, width, height);
					if (handle) {
						IMB_exr_read_channels(handle);
						ibuf->userdata = handle;         /* potential danger, the caller has to check for this! */
					}
				}
				else {
					FrameBuffer frameBuffer;
					float *first;
					int xstride = sizeof(float) * 4;
					int ystride = -xstride * width;

					imb_addrectfloatImBuf(ibuf);

					/* inverse correct first pixel for datawindow coordinates (- dw.min.y because of y flip) */
					first = ibuf->rect_float - 4 * (dw.min.x - dw.min.y * width);
					/* but, since we read y-flipped (negative y stride) we move to last scanline */
					first += 4 * (height - 1) * width;

					frameBuffer.insert(exr_rgba_channelname(file, "R"),
					                   Slice(Imf::FLOAT,  (char *) first, xstride, ystride));
					frameBuffer.insert(exr_rgba_channelname(file, "G"),
					                   Slice(Imf::FLOAT,  (char *) (first + 1), xstride, ystride));
					frameBuffer.insert(exr_rgba_channelname(file, "B"),
					                   Slice(Imf::FLOAT,  (char *) (first + 2), xstride, ystride));

					/* 1.0 is fill value, this still needs to be assigned even when (is_alpha == 0) */
					frameBuffer.insert(exr_rgba_channelname(file, "A"),
					                   Slice(Imf::FLOAT,  (char *) (first + 3), xstride, ystride, 1, 1, 1.0f));

					if (exr_has_zbuffer(file)) {
						float *firstz;

						addzbuffloatImBuf(ibuf);
						firstz = ibuf->zbuf_float - (dw.min.x - dw.min.y * width);
						firstz += (height - 1) * width;
						frameBuffer.insert("Z", Slice(Imf::FLOAT,  (char *)firstz, sizeof(float), -width * sizeof(float)));
					}

					InputPart in (*file, 0);
					in.setFrameBuffer(frameBuffer);
					in.readPixels(dw.min.y, dw.max.y);

					// XXX, ImBuf has no nice way to deal with this.
					// ideally IM_rect would be used when the caller wants a rect BUT
					// at the moment all functions use IM_rect.
					// Disabling this is ok because all functions should check if a rect exists and create one on demand.
					//
					// Disabling this because the sequencer frees immediate.
					//
					// if (flag & IM_rect)
					//     IMB_rect_from_float(ibuf);

					/* file is no longer needed */
					delete file;
				}
			}

			if (flags & IB_alphamode_detect)
				ibuf->flags |= IB_alphamode_premul;
		}
		return(ibuf);
	}
	catch (const std::exception &exc)
	{
		std::cerr << exc.what() << std::endl;
		if (ibuf) IMB_freeImBuf(ibuf);
		delete file;

		return (0);
	}

}

void imb_initopenexr(void)
{
	int num_threads = BLI_system_thread_count();

	setGlobalThreadCount(num_threads);
}

} // export "C"
