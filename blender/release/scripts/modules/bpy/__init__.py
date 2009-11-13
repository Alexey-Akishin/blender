# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
#
# ##### END GPL LICENSE BLOCK #####

# internal blender C module
import _bpy
from _bpy import types, props

data = _bpy.data
context = _bpy.context

# python modules
from bpy import utils
from bpy import ops as ops_module

# fake operator module
ops = ops_module.ops_fake_module

# load all scripts
import os
import sys

def load_scripts(reload_scripts=False):
    import traceback
    
    def test_import(module_name):
        try:
            return __import__(module_name)
        except:
            traceback.print_exc()
            return None
    
    base_path = os.path.join(os.path.dirname(__file__), "..", "..")
    base_path = os.path.normpath(base_path) # clean

    for path_subdir in ("ui", "op", "io"):
        path = os.path.join(base_path, path_subdir)
        sys.path.insert(0, path)
        for f in sorted(os.listdir(path)):
            if f.endswith(".py"):
                # python module
                mod = test_import(f[0:-3])
            elif "." not in f:
                # python package
                mod = test_import(f)
            else:
                mod = None
            
            if reload_scripts and mod:
                print("Reloading:", mod)
                reload(mod)

load_scripts()