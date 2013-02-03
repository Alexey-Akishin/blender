/*
 * ***** BEGIN GPL LICENSE BLOCK *****
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
 * Contributor(s): Geoffrey Bantle, Levi Schooley.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifndef __BMESH_H__
#define __BMESH_H__

/** \file blender/bmesh/bmesh.h
 *  \ingroup bmesh
 *
 * \addtogroup bmesh BMesh
 *
 * \brief BMesh is a non-manifold boundary representation designed to replace the current, limited EditMesh structure,
 * solving many of the design limitations and maintenance issues of EditMesh.
 *
 *
 * \section bm_structure The Structure
 *
 * BMesh stores topology in four main element structures:
 *
 * - Faces - BMFace
 * - Loops - BMLoop, (stores per-face-vertex data, UV's, vertex-colors, etc)
 * - Edges - BMEdge
 * - Verts - BMVert
 *
 *
 * \subsection bm_header_flags Header Flags
 * Each element (vertex/edge/face/loop) in a mesh has an associated bit-field called "header flags".
 *
 * BMHeader flags should <b>never</b> be read or written to by bmesh operators (see Operators below).
 *
 * Access to header flags is done with BM_elem_flag_*() functions.
 *
 *
 * \subsection bm_faces Faces
 *
 * Faces in BMesh are stored as a circular linked list of loops. Loops store per-face-vertex data
 * (amongst other things outlined later in this document), and define the face boundary.
 *
 *
 * \subsection bm_loop The Loop
 *
 * Loops define the boundary loop of a face. Each loop logically corresponds to an edge,
 * which is defined by the loop and next loop's vertices.
 *
 * Loops store several handy pointers:
 *
 * - BMLoop#v - pointer to the vertex associated with this loop.
 * - BMLoop#e - pointer to the edge associated with this loop.
 * - BMLoop#f - pointer to the face associated with this loop.
 *
 *
 * \subsection bm_two_side_face 2-Sided Faces
 *
 * There are some situations where you need 2-sided faces (e.g. a face of two vertices).
 * This is supported by BMesh, but note that such faces should only be used as intermediary steps,
 * and should not end up in the final mesh.
 *
 *
 * \subsection bm_edges_and_verts Edges and Vertices
 *
 * Edges and Vertices in BMesh are much like their counterparts in EditMesh,
 * except for some members private to the BMesh api.
 *
 * \note There can be more then one edge between two vertices in bmesh,
 * though the rest of blender (e.g. DerivedMesh, CDDM, CCGSubSurf, etc) does not support this.
 *
 *
 * \subsection bm_queries Queries
 *
 * The following topological queries are available:
 *
 * - Edges/Faces/Loops around a vertex.
 * - Faces around an edge.
 * - Loops around an edge.
 *
 * These are accessible through the iterator api, which is covered later in this document
 *
 * See source/blender/bmesh/bmesh_queries.h for more misc. queries.
 *
 *
 * \section bm_api The BMesh API
 *
 * One of the goals of the BMesh API is to make it easy and natural to produce highly maintainable code.
 * Code duplication, etc are avoided where possible.
 *
 *
 * \subsection bm_iter_api Iterator API
 *
 * Most topological queries in BMesh go through an iterator API (see Queries above).
 * These are defined in bmesh_iterators.h.  If you can, please use the #BM_ITER macro in bmesh_iterators.h
 *
 *
 * \subsection bm_walker_api Walker API
 *
 * Topological queries that require a stack (e.g. recursive queries) go through the Walker API,
 * which is defined in bmesh_walkers.h. Currently the "walkers" are hard-coded into the API,
 * though a mechanism for plugging in new walkers needs to be added at some point.
 *
 * Most topological queries should go through these two APIs;
 * there are additional functions you can use for topological iteration, but their meant for internal bmesh code.
 *
 * Note that the walker API supports delimiter flags, to allow the caller to flag elements not to walk past.
 *
 *
 * \subsection bm_ops Operators
 *
 * Operators are an integral part of BMesh. Unlike regular blender operators,
 * BMesh operators <b>bmo's</b> are designed to be nested (e.g. call other operators).
 *
 * Each operator has a number of input/output "slots" which are used to pass settings & data into/out of the operator
 * (and allows for chaining operators together).
 *
 * These slots are identified by name, using strings.
 *
 * Access to slots is done with BMO_slot_*() functions.
 *
 *
 * \subsection bm_tool_flags Tool Flags
 *
 * The BMesh API provides a set of flags for faces, edges and vertices, which are private to an operator.
 * These flags may be used by the client operator code as needed
 * (a common example is flagging elements for use in another operator).
 * Each call to an operator allocates it's own set of tool flags when it's executed,
 * avoiding flag conflicts between operators.
 *
 * These flags should not be confused with header flags, which are used to store persistent flags
 * (e.g. selection, hide status, etc).
 *
 * Access to tool flags is done with BMO_elem_flag_*() functions.
 *
 * \warning Operators are never allowed to read or write to header flags.
 * They act entirely on the data inside their input slots.
 * For example an operator should not check the selected state of an element,
 * there are some exceptions to this - some operators check of a face is smooth.
 *
 *
 * \subsection bm_slot_types Slot Types
 *
 * The following slot types are available:
 *
 * - integer - #BMO_OP_SLOT_INT
 * - boolean - #BMO_OP_SLOT_BOOL
 * - float   - #BMO_OP_SLOT_FLT
 * - pointer - #BMO_OP_SLOT_PNT
 * - element buffer - #BMO_OP_SLOT_ELEMENT_BUF - a list of verts/edges/faces
 * - map     - BMO_OP_SLOT_MAPPING - simple hash map
 *
 *
 * \subsection bm_slot_iter Slot Iterators
 *
 * Access to element buffers or maps must go through the slot iterator api, defined in bmesh_operators.h.
 * Use #BMO_ITER where ever possible.
 *
 *
 * \subsection bm_elem_buf Element Buffers
 *
 * The element buffer slot type is used to feed elements (verts/edges/faces) to operators.
 * Internally they are stored as pointer arrays (which happily has not caused any problems so far).
 * Many operators take in a buffer of elements, process it,
 * then spit out a new one; this allows operators to be chained together.
 *
 * \note Element buffers may have elements of different types within the same buffer (this is supported by the API.
 *
 *
 * \section bm_fname Function Naming Conventions
 *
 * These conventions should be used throughout the bmesh module.
 *
 * - BM_xxx() -     High level BMesh API function for use anywhere.
 * - bmesh_xxx() -  Low level API function.
 * - bm_xxx() -     'static' functions, not apart of the API at all, but use prefix since they operate on BMesh data.
 * - BMO_xxx() -    High level operator API function for use anywhere.
 * - bmo_xxx() -    Low level / internal operator API functions.
 * - _bm_xxx() -    Functions which are called via macros only.
 *
 * \section bm_todo BMesh TODO's
 *
 * There may be a better place for this section, but adding here for now.
 *
 * \subsection bm_todo_api API
 *
 * - make crease and bevel weight optional, they come for free in meshes but are allocated layers
 *   in the bmesh data structure.
 *
 *
 * \subsection bm_todo_tools Tools
 *
 * Probably most of these will be bmesh operators.
 *
 * - make ngons flat.
 * - make ngons into tris/quads (ngon poke?), many methods could be used here (triangulate/fan/quad-fan).
 * - solidify (precise mode), keeps even wall thickness, re-creates outlines of offset faces with plane-plane
 *   intersections.
 * - split vert (we already have in our API, just no tool)
 * - bridge (add option to bridge between different edge loop counts, option to remove selected face regions)
 * - flip selected region (invert all faces about the plane defined by the selected region outline)
 * - interactive dissolve (like the knife tool but draw over edges to dissolve)
 *
 *
 * \subsection bm_todo_optimize Optimizations
 *
 * - skip normal calc when its not needed (when calling chain of operators & for modifiers, flag as dirty)
 * - skip BMO flag allocation, its not needed in many cases, this is fairly redundant to calc by default.
 * - ability to call BMO's with option not to create return data (will save some time)
 * - binary diff UNDO, currently this uses huge amount of ram when all shapes are stored for each undo step for eg.
 * - use two differnt iterator types for BMO map/buffer types.
 * - avoid string lookups for BMO slot lookups _especially_ when used in loops, this is very crappy.
 *
 *
 * \subsection bm_todo_tools_enhance Tool Enhancements
 *
 * - face inset interpolate loop data from face (currently copies - but this stretches UV's in an ugly way)
 * - vert slide UV correction (like we have for edge slide)
 * - fill-face edge net - produce consistent normals, currently it won't, fix should be to fill in edge-net node
 *   connected with previous one - since they already check for normals of adjacent edge-faces before creating.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "DNA_listBase.h" /* selection history uses */
#include "DNA_customdata_types.h" /* BMesh struct in bmesh_class.h uses */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "bmesh_class.h"

/* include the rest of the API */
#include "intern/bmesh_operator_api.h"
#include "intern/bmesh_error.h"

#include "intern/bmesh_construct.h"
#include "intern/bmesh_core.h"
#include "intern/bmesh_interp.h"
#include "intern/bmesh_iterators.h"
#include "intern/bmesh_log.h"
#include "intern/bmesh_marking.h"
#include "intern/bmesh_mesh.h"
#include "intern/bmesh_mesh_conv.h"
#include "intern/bmesh_mesh_validate.h"
#include "intern/bmesh_mods.h"
#include "intern/bmesh_operators.h"
#include "intern/bmesh_polygon.h"
#include "intern/bmesh_queries.h"
#include "intern/bmesh_walkers.h"

#include "intern/bmesh_inline.h"

#include "tools/bmesh_bevel.h"
#include "tools/bmesh_decimate.h"
#include "tools/bmesh_triangulate.h"

#ifdef __cplusplus
}
#endif

#endif /* __BMESH_H__ */