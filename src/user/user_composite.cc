// Copyright 2021 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "user/user_api.h"
#include "user/user_composite.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <mujoco/mjmacro.h>
#include <mujoco/mjmodel.h>
#include <mujoco/mujoco.h>
#include "cc/array_safety.h"
#include "engine/engine_io.h"
#include "engine/engine_util_blas.h"
#include "engine/engine_util_errmem.h"
#include "engine/engine_util_misc.h"
#include "user/user_model.h"
#include "user/user_objects.h"
#include "user/user_util.h"
#include "xml/xml_util.h"

namespace {
namespace mju = ::mujoco::util;
using std::vector;
using std::string;
}  // namespace

// strncpy with 0, return false
static bool comperr(char* error, const char* msg, int error_sz) {
  mju_strncpy(error, msg, error_sz);
  return false;
}



// constructor
mjCComposite::mjCComposite(void) {
  // common properties
  prefix.clear();
  type = mjCOMPTYPE_PARTICLE;
  count[0] = count[1] = count[2] = 1;
  spacing = 0;
  mjuu_setvec(offset, 0, 0, 0);
  pin.clear();
  flatinertia = 0;
  mj_defaultSolRefImp(solrefsmooth, solimpsmooth);
  plugin_instance = nullptr;

  // cable
  curve[0] = curve[1] = curve[2] = mjCOMPSHAPE_ZERO;
  mjuu_setvec(size, 1, 0, 0);
  initial = "ball";

  // skin
  skin = false;
  skintexcoord = false;
  skinmaterial.clear();
  mjuu_setvec(skinrgba, 1, 1, 1, 1);
  skininflate = 0;
  skinsubgrid = 0;
  skingroup = 0;

  // clear add flags
  for (int i=0; i<mjNCOMPKINDS; i++) {
    add[i] = false;
  }

  // clear internal
  dim = 0;
}



// adjust constraint softness
void mjCComposite::AdjustSoft(mjtNum* solref, mjtNum* solimp, int level) {
  switch (level) {
  case 0:
    solref[0] = 0.01;
    solimp[0] = solimp[1] = 0.99;
    break;

  case 1:
    solref[0] = 0.02;
    solimp[0] = solimp[1] = 0.9;
    break;
  }
}



// create the array of default joint options, append new elements only for particles type
bool mjCComposite::AddDefaultJoint(char* error, int error_sz) {
  for (int i=0; i<mjNCOMPKINDS; i++) {
    if (!defjoint[(mjtCompKind)i].empty() && type!=mjCOMPTYPE_PARTICLE) {
      comperr(error, "Only particles are allowed to have multiple joints", error_sz);
      return false;
    } else {
      mjCDef jnt;
      jnt.joint.spec.group = 3;
      defjoint[(mjtCompKind)i].push_back(jnt);
    }
  }
  return true;
}



// set defaults, after reading top-level info and skin
void mjCComposite::SetDefault(void) {
  // determine dimensionality
  int tmpdim = 0;
  for (int i=0; i<3; i++) {
    if (count[i]>1) {
      tmpdim++;
    }
  }

  // set all deafult groups to 3
  for (int i=0; i<mjNCOMPKINDS; i++) {
    def[i].geom.spec.group = 3;
    def[i].site.spec.group = 3;
    def[i].tendon.spec.group = 3;
  }

  // set default joint
  AddDefaultJoint();

  // set default geom and tendon group to 0 if needed to be visible
  if (!skin ||
      type==mjCOMPTYPE_PARTICLE   ||
      type==mjCOMPTYPE_ROPE       ||
      type==mjCOMPTYPE_LOOP       ||
      type==mjCOMPTYPE_CABLE      ||
      (type==mjCOMPTYPE_GRID && tmpdim==1)) {
    for (int i=0; i<mjNCOMPKINDS; i++) {
      def[i].geom.spec.group = 0;
      def[i].tendon.spec.group = 0;
    }
  }

  // other type-specific adjustments
  switch (type) {
  case mjCOMPTYPE_PARTICLE:       // particle

    // no friction with anything
    def[0].geom.spec.condim = 1;
    def[0].geom.spec.priority = 1;
    break;

  case mjCOMPTYPE_GRID:           // grid

    // hard main tendon fix
    AdjustSoft(def[mjCOMPKIND_TENDON].equality.spec.solref,
               def[mjCOMPKIND_TENDON].equality.spec.solimp, 0);

    break;

  case mjCOMPTYPE_CABLE:          // cable
  case mjCOMPTYPE_ROPE:           // rope
    break;

  case mjCOMPTYPE_LOOP:           // loop

    // hard smoothing
    AdjustSoft(solrefsmooth, solimpsmooth, 0);
    break;

  case mjCOMPTYPE_CLOTH:          // cloth
    break;

  case mjCOMPTYPE_BOX:            // 3D
  case mjCOMPTYPE_CYLINDER:
  case mjCOMPTYPE_ELLIPSOID:

    // no self-collisions
    def[0].geom.spec.contype = 0;

    // soft smoothing
    AdjustSoft(solrefsmooth, solimpsmooth, 1);

    // soft fix everywhere
    for (int i=0; i<mjNCOMPKINDS; i++) {
      AdjustSoft(def[i].equality.spec.solref, def[i].equality.spec.solimp, 1);
    }

    // hard main tendon fix
    AdjustSoft(def[mjCOMPKIND_TENDON].equality.spec.solref,
               def[mjCOMPKIND_TENDON].equality.spec.solimp, 0);
    break;
  default:
    // SHOULD NOT OCCUR
    mju_error("Invalid composite type: %d", type);
    break;
  }
}



// make composite object
bool mjCComposite::Make(mjCModel* model, mjmBody* body, char* error, int error_sz) {
  // check geom type
  if ((def[0].geom.spec.type!=mjGEOM_SPHERE &&
       def[0].geom.spec.type!=mjGEOM_CAPSULE &&
       def[0].geom.spec.type!=mjGEOM_ELLIPSOID) &&
      type!=mjCOMPTYPE_PARTICLE && type!=mjCOMPTYPE_CABLE) {
    return comperr(error, "Composite geom type must be sphere, capsule or ellipsoid", error_sz);
  }

  // check pin coord number
  if (pin.size()%2) {
    return comperr(error, "Pin coordinate number of must be multiple of 2", error_sz);
  }

  // check counts
  for (int i=0; i<3; i++) {
    if (count[i]<1) {
      return comperr(error, "Positive counts expected in composite", error_sz);
    }
  }

  // check spacing
  if (type==mjCOMPTYPE_GRID || (type==mjCOMPTYPE_PARTICLE && uservert.empty())) {
    if (spacing < mju_max(def[0].geom.spec.size[0],
                  mju_max(def[0].geom.spec.size[1], def[0].geom.spec.size[2]))) {
      return comperr(error, "Spacing must be larger than geometry size",
                     error_sz);
    }
  }

  // check cable sizes are nonzero if vertices are not prescribed
  if (mjuu_dot3(size, size)<mjMINVAL && uservert.empty()) {
    return comperr(error, "Positive spacing or length expected in composite", error_sz);
  }

  // check spacing is not used by cable
  if (spacing && type==mjCOMPTYPE_CABLE) {
    return comperr(error, "Spacing is not supported by cable composite", error_sz);
  }

  // check either uservert or count but not both
  if (!uservert.empty()) {
    if (count[0]>1) {
      return comperr(error, "Either vertex or count can be specified, not both", error_sz);
    }
    count[0] = uservert.size()/3;
    count[1] = 1;
  }

  // determine dimensionality, check singleton order
  bool first = false;
  for (int i=0; i<3; i++) {
    if (count[i]==1) {
      first = true;
    } else {
      dim++;
      if (first) {
        return comperr(error, "Singleton counts must come last", error_sz);
      }
    }
  }

  // require 3x3 for subgrid
  if (skin && skinsubgrid>0 && type!=mjCOMPTYPE_CABLE) {
    if (count[0]<3 || count[1]<3) {
      return comperr(error, "At least 3x3 required for skin subgrid", error_sz);
    }
  }

  // dispatch
  switch (type) {
  case mjCOMPTYPE_PARTICLE:
    return MakeParticle(model, body, error, error_sz);

  case mjCOMPTYPE_GRID:
    return MakeGrid(model, body, error, error_sz);

  case mjCOMPTYPE_ROPE:
    return comperr(error,
                "The \"rope\" composite type is deprecated. Please use "
                "\"cable\" instead.",
                error_sz);

  case mjCOMPTYPE_LOOP:
    mju_warning(
        "The \"loop\" composite type is deprecated. Please use \"cable\" "
        "instead.");
    return MakeRope(model, body, error, error_sz);

  case mjCOMPTYPE_CABLE:
    return MakeCable(model, body, error, error_sz);

  case mjCOMPTYPE_CLOTH:
    return comperr(error,
                   "The \"cloth\" composite type is deprecated. Please use "
                   "\"shell\" instead.",
                   error_sz);

  case mjCOMPTYPE_BOX:
  case mjCOMPTYPE_CYLINDER:
  case mjCOMPTYPE_ELLIPSOID:
    return MakeBox(model, body, error, error_sz);

  default:
    return comperr(error, "Uknown shape in composite", error_sz);
  }
}



bool mjCComposite::MakeParticle(mjCModel* model, mjmBody* body, char* error, int error_sz) {
  char txt[100];
  std::vector<int> face;

  // populate vertices and names
  if (uservert.empty()) {
    if (spacing < mju_max(def[0].geom.spec.size[0],
                  mju_max(def[0].geom.spec.size[1], def[0].geom.spec.size[2])))
      return comperr(error, "Spacing must be larger than geometry size", error_sz);

    for (int ix=0; ix<count[0]; ix++) {
      for (int iy=0; iy<count[1]; iy++) {
        for (int iz=0; iz<count[2]; iz++) {
          uservert.push_back(spacing*(ix - 0.5*count[0]));
          uservert.push_back(spacing*(iy - 0.5*count[1]));
          uservert.push_back(spacing*(iz - 0.5*count[2]));

          mju::sprintf_arr(txt, "%sB%d_%d_%d", prefix.c_str(), ix, iy, iz);
          username.push_back(std::string(txt));
        }
      }
    }
  }

  // create faces
  if (userface.empty()) {
    if (dim == 3) {
      int cube2tets[6][4] = {{0, 3, 1, 7}, {0, 1, 4, 7},
                             {1, 3, 2, 7}, {1, 2, 6, 7},
                             {1, 5, 4, 7}, {1, 6, 5, 7}};
      for (int ix = 0; ix < count[0]-1; ix++) {
        for (int iy = 0; iy < count[1]-1; iy++) {
          for (int iz = 0; iz < count[2]-1; iz++) {
            int vert[8] = {
              count[2]*count[1]*(ix+0) + count[2]*(iy+0) + iz+0,
              count[2]*count[1]*(ix+1) + count[2]*(iy+0) + iz+0,
              count[2]*count[1]*(ix+1) + count[2]*(iy+1) + iz+0,
              count[2]*count[1]*(ix+0) + count[2]*(iy+1) + iz+0,
              count[2]*count[1]*(ix+0) + count[2]*(iy+0) + iz+1,
              count[2]*count[1]*(ix+1) + count[2]*(iy+0) + iz+1,
              count[2]*count[1]*(ix+1) + count[2]*(iy+1) + iz+1,
              count[2]*count[1]*(ix+0) + count[2]*(iy+1) + iz+1,
            };
            for (int s = 0; s < 6; s++) {
              for (int v = 0; v < 4; v++) {
                face.push_back(vert[cube2tets[s][v]]);
              }
            }
          }
        }
      }
    } else if (dim == 2) {
      int quad2tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
      for (int ix = 0; ix < count[0]-1; ix++) {
        for (int iy = 0; iy < count[1]-1; iy++) {
          int vert[4] = {
            count[2]*count[1]*(ix+0) + count[2]*(iy+0),
            count[2]*count[1]*(ix+1) + count[2]*(iy+0),
            count[2]*count[1]*(ix+1) + count[2]*(iy+1),
            count[2]*count[1]*(ix+0) + count[2]*(iy+1),
          };
          for (int s = 0; s < 2; s++) {
            for (int v = 0; v < 3; v++) {
              face.push_back(vert[quad2tri[s][v]]);
            }
          }
        }
      }
    }
    mjXUtil::Vector2String(userface, face);
    } else {
    dim = 2;  // can only load a surface for now
    mjXUtil::String2Vector(userface, face);
    for (int i=0; i<face.size(); face[i++]--) {};
    mjXUtil::Vector2String(userface, face);
  }

  // compute volume
  std::vector<mjtNum> volume(uservert.size()/3);
  mjtNum t = 1;
  if (dim == 2 && plugin_instance) {
    try {
      t = std::stod(plugin_instance->config_attribs["thickness"], nullptr);
    } catch (const std::invalid_argument& e) {
      return comperr(error, "Invalid thickness attribute", error_sz);
    }
  }
  if (!userface.empty()) {
    mjXUtil::String2Vector(userface, face);
    for (int j=0; j<face.size()/3; j++) {
      mjtNum area[3];
      mjtNum edge1[3];
      mjtNum edge2[3];

      for (int i=0; i<3; i++) {
        edge1[i] = uservert[3*face[3*j+1]+i] - uservert[3*face[3*j]+i];
        edge2[i] = uservert[3*face[3*j+2]+i] - uservert[3*face[3*j]+i];
      }

      mjuu_crossvec(area, edge1, edge2);
      for (int i=0; i<3; i++) {
        volume[face[3*j+i]] += sqrt(mjuu_dot3(area, area)) / 2 * t;
      }
    }
  } else {
    for (int i=0; i<uservert.size()/3; i++) {
      volume[i] = 6 * spacing * spacing / 2 * t;
    }
  }

  // create bodies and geoms
  for (int i=0; i<uservert.size()/3; i++) {
    // create body
    mjmBody* b = mjm_addBody(body, NULL);

    if (!username.empty()) {
      mjm_setString(b->name, username[i].c_str());
    } else {
      mju::sprintf_arr(txt, "%sB%d", prefix.c_str(), i);
      mjm_setString(b->name, txt);
    }

    // set body position
    b->pos[0] = offset[0] + uservert[3*i];
    b->pos[1] = offset[1] + uservert[3*i+1];
    b->pos[2] = offset[2] + uservert[3*i+2];

    // add slider joints if none defined
    if (!add[mjCOMPKIND_PARTICLE]) {
      for (int i=0; i<3; i++) {
        mjmJoint* jnt = mjm_addJoint(b, &defjoint[mjCOMPKIND_JOINT][0]);
        mjm_setDefault(jnt->element, mjm_getDefault(body->element));
        jnt->type = mjJNT_SLIDE;
        mjuu_setvec(jnt->pos, 0, 0, 0);
        mjuu_setvec(jnt->axis, 0, 0, 0);
        jnt->axis[i] = 1;
      }
    }

    // add user-specified joints
    else {
      for (auto defjnt : defjoint[mjCOMPKIND_PARTICLE]) {
        mjmJoint* jnt = mjm_addJoint(b, &defjnt);
        mjm_setDefault(jnt->element, mjm_getDefault(body->element));
      }
    }

    // add geom
    mjmGeom* g = mjm_addGeom(b, def);
    mjm_setDefault(g->element, mjm_getDefault(body->element));

    // add site
    mjmSite* s = mjm_addSite(b, def);
    mjm_setDefault(s->element, mjm_getDefault(body->element));
    s->type = mjGEOM_SPHERE;
    mju::sprintf_arr(txt, "%sS%d", prefix.c_str(), i);
    mjm_setString(s->name, txt);

    // add plugin
    if (plugin_instance) {
      mjmPlugin* plugin = &b->plugin;
      plugin->active = true;
      plugin->instance = (mjElement)plugin_instance;
      mjm_setString(plugin->instance_name, plugin_instance_name.c_str());
      mjm_setString(plugin->name, plugin_name.c_str());

      if (i==0 && !plugin_instance->config_attribs["face"].empty()) {
        return comperr(error, "Face attribute already exists in plugin", error_sz);
      }

      plugin_instance->config_attribs["face"] = userface;
      plugin_instance->config_attribs["edge"] = "";

      // update density
      if (dim == 2) {
        g->density *= volume[i] / (4./3. * mjPI * pow(g->size[0], 3));
      }
    }
  }

  // add isometry constraints
  if (dim==2) {
    char txt0[100], txt1[100], txt2[100];
    std::vector<std::pair<int, int>> edge;

    // create edges
    for (int i=0; i<face.size()/3; i++) {
      for (int j=0; j<3; j++) {
        int v0 = face[3*i+(j+0)%3];
        int v1 = face[3*i+(j+1)%3];
        edge.push_back(v0 < v1 ? std::pair(v0, v1) : std::pair(v1, v0));
      }
    }

    std::sort(edge.begin(), edge.end());
    auto last = std::unique(edge.begin(), edge.end());
    edge.erase(last, edge.end());

    // create constraints
    for (int i=0; i<edge.size(); i++) {
      int v0 = edge[i].first;
      int v1 = edge[i].second;

      mju::sprintf_arr(txt0, "%sT%d_%d", prefix.c_str(), v0, v1);
      mju::sprintf_arr(txt1, "%sS%d", prefix.c_str(), v0);
      mju::sprintf_arr(txt2, "%sS%d", prefix.c_str(), v1);

      // create tendon
      mjmTendon* ten = mjm_addTendon(model, def + mjCOMPKIND_TENDON);
      mjm_setDefault(ten->element, model->defaults[0]);
      mjm_setString(ten->name, txt0);
      ten->group = 4;
      mjm_wrapSite(ten, txt1);
      mjm_wrapSite(ten, txt2);

      // add equality constraint
      mjmEquality* eq = mjm_addEquality(model, def + mjCOMPKIND_TENDON);
      mjm_setDefault(eq->element, model->defaults[0]);
      eq->type = mjEQ_TENDON;
      mjm_setString(eq->name1, mjm_getString(ten->name));
    }
  }

  if (skin && dim==3) {
    MakeSkin3(model);
  }

  if (skin && dim==2) {
    if (skinsubgrid>0) {
      MakeSkin2Subgrid(model, skininflate);
    } else {
      MakeSkin2(model, skininflate);
    }
  }

  return true;
}



// make grid connected with tendons
bool mjCComposite::MakeGrid(mjCModel* model, mjmBody* body, char* error, int error_sz) {
  char txt[100], txt1[100], txt2[100];

  // check dimensionality
  if (dim>2) {
    return comperr(error, "Grid can only be 1D or 2D", error_sz);
  }

  // check shear dimensionality
  if (add[mjCOMPKIND_SHEAR] && dim!=2) {
    return comperr(error, "Shear requires 2D grid", error_sz);
  }

  // check skin dimensionality
  if (skin && dim!=2) {
    return comperr(error, "Skin requires 2D grid", error_sz);
  }

  // create bodies, joints, geoms, sites
  for (int ix=0; ix<count[0]; ix++) {
    for (int iy=0; iy<count[1]; iy++) {
      // create body
      mjmBody* b = mjm_addBody(body, NULL);
      mju::sprintf_arr(txt, "%sB%d_%d", prefix.c_str(), ix, iy);
      mjm_setString(b->name, txt);

      // set body position
      b->pos[0] = offset[0] + spacing*(ix - 0.5*count[0]);
      b->pos[1] = offset[1] + spacing*(iy - 0.5*count[1]);
      b->pos[2] = offset[2];

      // add geom
      mjmGeom* g = mjm_addGeom(b, def);
      mjm_setDefault(g->element, mjm_getDefault(body->element));
      g->type = mjGEOM_SPHERE;
      mju::sprintf_arr(txt, "%sG%d_%d", prefix.c_str(), ix, iy);
      mjm_setString(g->name, txt);

      // add site
      mjmSite* s = mjm_addSite(b, def);
      mjm_setDefault(s->element, mjm_getDefault(body->element));
      s->type = mjGEOM_SPHERE;
      mju::sprintf_arr(txt, "%sS%d_%d", prefix.c_str(), ix, iy);
      mjm_setString(s->name, txt);

      // skip pinned elements
      bool skip = false;
      for (int ip=0; ip<pin.size(); ip+=2) {
        if (pin[ip]==ix && pin[ip+1]==iy) {
          skip = true;
          break;
        }
      }
      if (skip) {
        continue;
      }

      // add slider joint
      mjmJoint* jnt[3];
      for (int i=0; i<3; i++) {
        jnt[i] = mjm_addJoint(b, &defjoint[mjCOMPKIND_JOINT][0]);
        mjm_setDefault(jnt[i]->element, mjm_getDefault(body->element));
        mju::sprintf_arr(txt, "%sJ%d_%d_%d", prefix.c_str(), i, ix, iy);
        mjm_setString(jnt[i]->name, txt);
        jnt[i]->type = mjJNT_SLIDE;
        mjuu_setvec(jnt[i]->pos, 0, 0, 0);
        mjuu_setvec(jnt[i]->axis, 0, 0, 0);
        jnt[i]->axis[i] = 1;
      }
    }
  }

  // create tendons and equality constraints
  for (int i=0; i<2; i++) {
    for (int ix=0; ix<count[0]-(i==0); ix++) {
      for (int iy=0; iy<count[1]-(i==1); iy++) {
        // recover site names
        mju::sprintf_arr(txt1, "%sS%d_%d", prefix.c_str(), ix, iy);
        mju::sprintf_arr(txt2, "%sS%d_%d", prefix.c_str(), ix+(i==0), iy+(i==1));

        // create tendon
        mjCTendon* ten = model->AddTendon(def + mjCOMPKIND_TENDON);
        ten->def = model->defaults[0];
        mju::sprintf_arr(txt, "%sT%d_%d_%d", prefix.c_str(), i, ix, iy);
        ten->name = txt;
        ten->WrapSite(txt1);
        ten->WrapSite(txt2);

        // add equality constraint
        mjmEquality* eq = mjm_addEquality(model, def + mjCOMPKIND_TENDON);
        mjm_setDefault(eq->element, model->defaults[0]);
        eq->type = mjEQ_TENDON;
        mjm_setString(eq->name1, ten->name.c_str());
      }
    }
  }

  // shear for 2D
  if (add[mjCOMPKIND_SHEAR]) {
    MakeShear(model);
  }

  // skin
  if (skin) {
    if (skinsubgrid>0) {
      MakeSkin2Subgrid(model, skininflate);
    } else {
      MakeSkin2(model, skininflate);
    }
  }

  return true;
}



bool mjCComposite::MakeCable(mjCModel* model, mjmBody* body, char* error, int error_sz) {
  // check dim
  if (dim!=1) {
    return comperr(error, "Cable must be one-dimensional", error_sz);
  }

  // check geom type
  if (def[0].geom.spec.type!=mjGEOM_CYLINDER &&
      def[0].geom.spec.type!=mjGEOM_CAPSULE &&
      def[0].geom.spec.type!=mjGEOM_BOX) {
    return comperr(error, "Cable geom type must be sphere, capsule or box", error_sz);
  }

  // add name to model
  mjmText* pte = mjm_addText(model);
  mjm_setString(pte->name, ("composite_" + prefix).c_str());
  mjm_setString(pte->data, ("rope_" + prefix).c_str());

  // populate uservert if not specified
  if (uservert.empty()) {
    for (int ix=0; ix<count[0]; ix++) {
      for (int k=0; k<3; k++) {
        switch (curve[k]) {
        case mjCOMPSHAPE_LINE:
          uservert.push_back(ix*size[0]/(count[0]-1));
          break;
        case mjCOMPSHAPE_COS:
          uservert.push_back(size[1]*cos(mjPI*ix*size[2]/(count[0]-1)));
          break;
        case mjCOMPSHAPE_SIN:
          uservert.push_back(size[1]*sin(mjPI*ix*size[2]/(count[0]-1)));
          break;
        case mjCOMPSHAPE_ZERO:
          uservert.push_back(0);
          break;
        default:
          // SHOULD NOT OCCUR
          mju_error("Invalid composite shape: %d", curve[k]);
          break;
        }
      }
    }
  }

  // create frame
  mjtNum normal[3], prev_quat[4];
  mjuu_setvec(normal, 0, 1, 0);
  mjuu_setvec(prev_quat, 1, 0, 0, 0);

  // add one body after the other
  for (int ix=0; ix<count[0]-1; ix++) {
    body = AddCableBody(model, body, ix, normal, prev_quat);
  }

  // add skin
  if (def[0].geom.spec.type==mjGEOM_BOX) {
    if (skinsubgrid>0) {
      count[1]+=2;
      MakeSkin2Subgrid(model, 2*def[0].geom.spec.size[2]);
      count[1]-=2;
    } else {
      count[1]++;
      MakeSkin2(model, 2*def[0].geom.spec.size[2]);
      count[1]--;
    }
  }
  return true;
}



mjmBody* mjCComposite::AddCableBody(mjCModel* model, mjmBody* body, int ix, mjtNum normal[3], mjtNum prev_quat[4]) {
  char txt_geom[100], txt_site[100], txt_slide[100];
  char this_body[100], next_body[100], this_joint[100];
  mjtNum dquat[4], this_quat[4];

  // set flags
  int lastidx = count[0]-2;
  bool first = ix==0;
  bool last = ix==lastidx;
  bool secondlast = ix==lastidx-1;

  // compute edge and tangent vectors
  mjtNum edge[3], tprev[3], tnext[3];
  mjuu_setvec(edge, uservert[3*(ix+1)+0]-uservert[3*ix+0],
                    uservert[3*(ix+1)+1]-uservert[3*ix+1],
                    uservert[3*(ix+1)+2]-uservert[3*ix+2]);
  if (!first) {
    mjuu_setvec(tprev, uservert[3*ix+0]-uservert[3*(ix-1)+0],
                       uservert[3*ix+1]-uservert[3*(ix-1)+1],
                       uservert[3*ix+2]-uservert[3*(ix-1)+2]);
    mjuu_normvec(tprev, 3);
  }
  if (!last) {
    mjuu_setvec(tnext, uservert[3*(ix+2)+0]-uservert[3*(ix+1)+0],
                       uservert[3*(ix+2)+1]-uservert[3*(ix+1)+1],
                       uservert[3*(ix+2)+2]-uservert[3*(ix+1)+2]);
    mjuu_normvec(tnext, 3);
  }

  // update moving frame
  mjtNum length = mju_updateFrame(this_quat, normal, edge, tprev, tnext, first);

  // create body, joint, and geom names
  if (first) {
    mju::sprintf_arr(this_body, "%sB_first", prefix.c_str());
    mju::sprintf_arr(next_body, "%sB_%d", prefix.c_str(), ix+1);
    mju::sprintf_arr(this_joint, "%sJ_first", prefix.c_str());
    mju::sprintf_arr(txt_site, "%sS_first", prefix.c_str());
  } else if (last) {
    mju::sprintf_arr(this_body, "%sB_last", prefix.c_str());
    mju::sprintf_arr(next_body, "%sB_first", prefix.c_str());
    mju::sprintf_arr(this_joint, "%sJ_last", prefix.c_str());
    mju::sprintf_arr(txt_site, "%sS_last", prefix.c_str());
  } else if (secondlast){
    mju::sprintf_arr(this_body, "%sB_%d", prefix.c_str(), ix);
    mju::sprintf_arr(next_body, "%sB_last", prefix.c_str());
    mju::sprintf_arr(this_joint, "%sJ_%d", prefix.c_str(), ix);
  } else {
    mju::sprintf_arr(this_body, "%sB_%d", prefix.c_str(), ix);
    mju::sprintf_arr(next_body, "%sB_%d", prefix.c_str(), ix+1);
    mju::sprintf_arr(this_joint, "%sJ_%d", prefix.c_str(), ix);
  }
  mju::sprintf_arr(txt_geom, "%sG%d", prefix.c_str(), ix);
  mju::sprintf_arr(txt_slide, "%sJs%d", prefix.c_str(), ix);

  // add body
  body = mjm_addBody(body, 0);
  mjm_setString(body->name, this_body);
  if (first) {
    mjuu_setvec(body->pos, offset[0]+uservert[3*ix],
                           offset[1]+uservert[3*ix+1],
                           offset[2]+uservert[3*ix+2]);
    mjuu_copyvec(body->quat, this_quat, 4);
  } else {
    mjuu_setvec(body->pos, length, 0, 0);
    mjtNum negquat[4] = {prev_quat[0], -prev_quat[1], -prev_quat[2], -prev_quat[3]};
    mjuu_mulquat(dquat, negquat, this_quat);
    mjuu_copyvec(body->quat, dquat, 4);
  }

  // add geom
  mjmGeom* geom = mjm_addGeom(body, def);
  mjm_setDefault(geom->element, mjm_getDefault(body->element));
  mjm_setString(geom->name, txt_geom);
  if (def[0].geom.spec.type==mjGEOM_CYLINDER ||
      def[0].geom.spec.type==mjGEOM_CAPSULE) {
    mjuu_zerovec(geom->fromto, 6);
    geom->fromto[3] = length;
  } else if (def[0].geom.spec.type==mjGEOM_BOX) {
    mjuu_zerovec(geom->pos, 3);
    geom->pos[0] = length/2;
    geom->size[0] = length/2;
  }

  // add plugin
  if (plugin_instance) {
    mjmPlugin* plugin = &body->plugin;
    plugin->active = true;
    plugin->instance = (mjElement)plugin_instance;
    mjm_setString(plugin->name, plugin_name.c_str());
    mjm_setString(plugin->instance_name, plugin_instance_name.c_str());
  }

  // update orientation
  mjuu_copyvec(prev_quat, this_quat, 4);

  // add curvature joint
  if (!first || strcmp(initial.c_str(), "none")) {
    mjmJoint* jnt = mjm_addJoint(body, &defjoint[mjCOMPKIND_JOINT][0]);
    mjm_setDefault(jnt->element, mjm_getDefault(body->element));
    jnt->type = (first && strcmp(initial.c_str(), "free")==0) ? mjJNT_FREE : mjJNT_BALL;
    jnt->damping = jnt->type==mjJNT_FREE ? 0 : jnt->damping;
    jnt->armature = jnt->type==mjJNT_FREE ? 0 : jnt->armature;
    jnt->frictionloss = jnt->type==mjJNT_FREE ? 0 : jnt->frictionloss;
    mjm_setString(jnt->name, this_joint);
  }

  // exclude contact pair
  if (!last) {
    mjmExclude* exclude = mjm_addExclude(model);
    mjm_setString(exclude->bodyname1, std::string(this_body).c_str());
    mjm_setString(exclude->bodyname2, std::string(next_body).c_str());
  }

  // add site at the boundary
  if (last || first) {
    mjmSite* site = mjm_addSite(body, def);
    mjm_setDefault(site->element, mjm_getDefault(body->element));
    mjm_setString(site->name, txt_site);
    mjuu_setvec(site->pos, last ? length : 0, 0, 0);
    mjuu_setvec(site->quat, 1, 0, 0, 0);
  }

  return body;
}


// make rope
bool mjCComposite::MakeRope(mjCModel* model, mjmBody* body, char* error, int error_sz) {
  // check dim
  if (dim!=1) {
    return comperr(error, "Rope must be one-dimensional", error_sz);
  }

  // check root body name prefix
  char txt[200];
  mju::sprintf_arr(txt, "%sB", prefix.c_str());
  std::string body_name = mjm_getString(body->name);
  if (std::strncmp(txt, body_name.substr(0, strlen(txt)).c_str(), mju::sizeof_arr(txt))) {
    mju::strcat_arr(txt, " must be the beginning of root body name");
    return comperr(error, txt, error_sz);
  }

  // read origin coordinate from root body
  mju::strcpy_arr(txt, body_name.substr(strlen(txt)).c_str());
  int ox = -1;
  if (sscanf(txt, "%d", &ox)!=1) {
    return comperr(error, "Root body name must contain X coordinate", error_sz);
  }
  if (ox<0 || ox>=count[0]) {
    return comperr(error, "Root body coordinate out of range", error_sz);
  }

  // add origin
  AddRopeBody(model, body, ox, ox);

  // add elements: right
  mjmBody* pbody = body;
  for (int ix=ox; ix<count[0]-1; ix++) {
    pbody = AddRopeBody(model, pbody, ix, ix+1);
  }

  // add elements: left
  pbody = body;
  for (int ix=ox; ix>0; ix--) {
    pbody = AddRopeBody(model, pbody, ix, ix-1);
  }

  // close loop
  if (type==mjCOMPTYPE_LOOP) {
    char txt2[200];

    // add equality constraint
    mjmEquality* eq = mjm_addEquality(model, 0);
    eq->type = mjEQ_CONNECT;
    mju::sprintf_arr(txt, "%sB0", prefix.c_str());
    mju::sprintf_arr(txt2, "%sB%d", prefix.c_str(), count[0]-1);
    mjm_setString(eq->name1, txt);
    mjm_setString(eq->name2, txt2);
    mjuu_setvec(eq->data, -0.5*spacing, 0, 0);
    mju_copy(eq->solref, solrefsmooth, mjNREF);
    mju_copy(eq->solimp, solimpsmooth, mjNIMP);

    // remove contact between connected bodies
    mjmExclude* pair = mjm_addExclude(model);
    mjm_setString(pair->bodyname1, std::string(txt).c_str());
    mjm_setString(pair->bodyname2, std::string(txt2).c_str());
  }

  return true;
}



// add child body for cloth
mjmBody* mjCComposite::AddRopeBody(mjCModel* model, mjmBody* body, int ix, int ix1) {
  char txt[100];
  bool isroot = (ix==ix1);
  double dx = spacing*(ix1-ix);

  // add child if not root
  if (!isroot) {
    body = mjm_addBody(body, 0);
    mju::sprintf_arr(txt, "%sB%d", prefix.c_str(), ix1);
    mjm_setString(body->name, txt);

    // loop
    if (type==mjCOMPTYPE_LOOP) {
      double alpha = 2*mjPI/count[0];
      double R = 0.5*spacing*sin(mjPI-alpha)/sin(0.5*alpha);

      if (ix1>ix) {
        mjuu_setvec(body->pos, R*cos(0.5*alpha), R*sin(0.5*alpha), 0);
        mjuu_setvec(body->quat, cos(0.5*alpha), 0, 0, sin(0.5*alpha));
      } else {
        mjuu_setvec(body->pos, -R*cos(0.5*alpha), R*sin(0.5*alpha), 0);
        mjuu_setvec(body->quat, cos(-0.5*alpha), 0, 0, sin(-0.5*alpha));
      }
    }

    // no loop
    else {
      mjuu_setvec(body->pos, dx, 0, 0);
    }
  }

  // add geom
  mjmGeom* geom = mjm_addGeom(body, def);
  mjm_setDefault(geom->element, mjm_getDefault(body->element));
  mju::sprintf_arr(txt, "%sG%d", prefix.c_str(), ix1);
  mjm_setString(geom->name, txt);
  mjuu_setvec(geom->pos, 0, 0, 0);
  mjuu_setvec(geom->quat, sqrt(0.5), 0, sqrt(0.5), 0);

  // root: no joints
  if (isroot) {
    return body;
  }

  // add main joint
  for (int i=0; i<2; i++) {
    // add joint
    mjmJoint* jnt = mjm_addJoint(body, &defjoint[mjCOMPKIND_JOINT][0]);
    mjm_setDefault(jnt->element, mjm_getDefault(body->element));
    mju::sprintf_arr(txt, "%sJ%d_%d", prefix.c_str(), i, ix1);
    mjm_setString(jnt->name, txt);
    jnt->type = mjJNT_HINGE;
    mjuu_setvec(jnt->pos, -0.5*dx, 0, 0);
    mjuu_setvec(jnt->axis, 0, 0, 0);
    jnt->axis[i+1] = 1;
  }

  // add twist joint
  if (add[mjCOMPKIND_TWIST]) {
    // add joint
    mjmJoint* jnt = mjm_addJoint(body, &defjoint[mjCOMPKIND_TWIST][0]);
    mjm_setDefault(jnt->element, mjm_getDefault(body->element));
    mju::sprintf_arr(txt, "%sJT%d", prefix.c_str(), ix1);
    mjm_setString(jnt->name, txt);
    jnt->type = mjJNT_HINGE;
    mjuu_setvec(jnt->pos, -0.5*dx, 0, 0);
    mjuu_setvec(jnt->axis, 1, 0, 0);

    // add constraint
    mjmEquality* eq = mjm_addEquality(model, def + mjCOMPKIND_TWIST);
    mjm_setDefault(eq->element, model->defaults[0]);
    eq->type = mjEQ_JOINT;
    mjm_setString(eq->name1, mjm_getString(jnt->name));
  }

  // add stretch joint
  if (add[mjCOMPKIND_STRETCH]) {
    // add joint
    mjmJoint* jnt = mjm_addJoint(body, &defjoint[mjCOMPKIND_STRETCH][0]);
    mjm_setDefault(jnt->element, mjm_getDefault(body->element));
    mju::sprintf_arr(txt, "%sJS%d", prefix.c_str(), ix1);
    mjm_setString(jnt->name, txt);
    jnt->type = mjJNT_SLIDE;
    mjuu_setvec(jnt->pos, -0.5*dx, 0, 0);
    mjuu_setvec(jnt->axis, 1, 0, 0);

    // add constraint
    mjmEquality* eq = mjm_addEquality(model, def + mjCOMPKIND_STRETCH);
    mjm_setDefault(eq->element, model->defaults[0]);
    eq->type = mjEQ_JOINT;
    mjm_setString(eq->name1, mjm_getString(jnt->name));
  }

  return body;
}



// project from box to other shape
void mjCComposite::BoxProject(double* pos) {
  // determine sizes
  double size[3] = {
    0.5*spacing*(count[0]-1),
    0.5*spacing*(count[1]-1),
    0.5*spacing*(count[2]-1)
  };

  // box
  if (type==mjCOMPTYPE_BOX) {
    pos[0] *= size[0];
    pos[1] *= size[1];
    pos[2] *= size[2];
  }

  // cylinder
  else if (type==mjCOMPTYPE_CYLINDER) {
    double L0 = mju_max(mju_abs(pos[0]), mju_abs(pos[1]));
    mjuu_normvec(pos, 2);
    pos[0] *= size[0]*L0;
    pos[1] *= size[1]*L0;
    pos[2] *= size[2];
  }

  // ellipsoid
  else if (type==mjCOMPTYPE_ELLIPSOID) {
    mjuu_normvec(pos, 3);
    pos[0] *= size[0];
    pos[1] *= size[1];
    pos[2] *= size[2];
  }
}



// make 3d box, ellipsoid or cylinder
bool mjCComposite::MakeBox(mjCModel* model, mjmBody* body, char* error, int error_sz) {
  char txt[100];

  // check dim
  if (dim!=3) {
    return comperr(error, "Box and ellipsoid must be three-dimensional", error_sz);
  }

  // center geom: two times bigger
  mjmGeom* geom = mjm_addGeom(body, def);
  mjm_setDefault(geom->element, mjm_getDefault(body->element));
  geom->type = mjGEOM_SPHERE;
  mju::sprintf_arr(txt, "%sGcenter", prefix.c_str());
  mjm_setString(geom->name, txt);
  mjuu_setvec(geom->pos, 0, 0, 0);
  geom->size[0] *= 2;
  geom->size[1] = 0;
  geom->size[2] = 0;

  // fixed tendon for all joints
  mjCTendon* ten = model->AddTendon(def + mjCOMPKIND_TENDON);
  ten->def = model->defaults[0];
  mju::sprintf_arr(txt, "%sT", prefix.c_str());
  ten->name = txt;

  // create bodies, geoms and joints: outside shell only
  for (int ix=0; ix<count[0]; ix++) {
    for (int iy=0; iy<count[1]; iy++) {
      for (int iz=0; iz<count[2]; iz++) {
        if (ix==0 || ix==count[0]-1 ||
            iy==0 || iy==count[1]-1 ||
            iz==0 || iz==count[2]-1) {
          // create body
          mjmBody* b = mjm_addBody(body, NULL);
          mju::sprintf_arr(txt, "%sB%d_%d_%d", prefix.c_str(), ix, iy, iz);
          mjm_setString(b->name, txt);

          // set body position (+/- 1)
          b->pos[0] = 2.0*ix/(count[0]-1) - 1;
          b->pos[1] = 2.0*iy/(count[1]-1) - 1;
          b->pos[2] = 2.0*iz/(count[2]-1) - 1;

          // reshape
          BoxProject(b->pos);

          // reorient body
          mjuu_copyvec(b->alt.zaxis, b->pos, 3);
          mjuu_normvec(b->alt.zaxis, 3);

          // add geom
          mjmGeom* g = mjm_addGeom(b, def);
          mjm_setDefault(g->element, mjm_getDefault(body->element));
          mju::sprintf_arr(txt, "%sG%d_%d_%d", prefix.c_str(), ix, iy, iz);
          mjm_setString(g->name, txt);

          // offset inwards, enforce sphere or capsule
          if (g->type==mjGEOM_CAPSULE) {
            g->pos[2] = -(g->size[0] + g->size[1]);
          } else {
            g->type = mjGEOM_SPHERE;
            g->pos[2] = -g->size[0];
          }

          // add slider joint
          mjmJoint* jnt = mjm_addJoint(b, &defjoint[mjCOMPKIND_JOINT][0]);
          mjm_setDefault(jnt->element, mjm_getDefault(body->element));
          mju::sprintf_arr(txt, "%sJ%d_%d_%d", prefix.c_str(), ix, iy, iz);
          mjm_setString(jnt->name, txt);
          jnt->type = mjJNT_SLIDE;
          mjuu_setvec(jnt->pos, 0, 0, 0);
          mjuu_setvec(jnt->axis, 0, 0, 1);

          // add fix constraint
          mjmEquality* eq = mjm_addEquality(model, def + mjCOMPKIND_JOINT);
          mjm_setDefault(eq->element, model->defaults[0]);
          eq->type = mjEQ_JOINT;
          mjm_setString(eq->name1, mjm_getString(jnt->name));

          // add joint to tendon
          ten->WrapJoint(std::string(mjm_getString(jnt->name)), 1);

          // add neighbor constraints
          for (int i=0; i<3; i++) {
            int ix1 = mjMIN(ix+(i==0), count[0]-1);
            int iy1 = mjMIN(iy+(i==1), count[1]-1);
            int iz1 = mjMIN(iz+(i==2), count[2]-1);
            if ((ix1==0 || ix1==count[0]-1 ||
                 iy1==0 || iy1==count[1]-1 ||
                 iz1==0 || iz1==count[2]-1) &&
                (ix!=ix1 || iy!=iy1 || iz!=iz1)) {
              char txt2[200];
              mju::sprintf_arr(txt2,
                               "%sJ%d_%d_%d", prefix.c_str(), ix1, iy1, iz1);
              mjmEquality* eqn = mjm_addEquality(model, 0);
              mju_copy(eqn->solref, solrefsmooth, mjNREF);
              mju_copy(eqn->solimp, solimpsmooth, mjNIMP);
              eqn->type = mjEQ_JOINT;
              mjm_setString(eqn->name1, txt);
              mjm_setString(eqn->name2, txt2);
            }
          }
        }
      }
    }
  }

  // finalize fixed tendon
  mjmEquality* eqt = mjm_addEquality(model, def + mjCOMPKIND_TENDON);
  mjm_setDefault(eqt->element, model->defaults[0]);
  eqt->type = mjEQ_TENDON;
  mjm_setString(eqt->name1, ten->name.c_str());

  // skin
  if (skin) {
    MakeSkin3(model);
  }

  return true;
}



// add shear tendons to 2D
void mjCComposite::MakeShear(mjCModel* model) {
  char txt[100], txt1[100], txt2[100];

  for (int ix=0; ix<count[0]-1; ix++) {
    for (int iy=0; iy<count[1]-1; iy++) {
      // recover site names
      mju::sprintf_arr(txt1, "%sS%d_%d", prefix.c_str(), ix, iy);
      mju::sprintf_arr(txt2, "%sS%d_%d", prefix.c_str(), ix+1, iy+1);

      // create tendon
      mjCTendon* ten = model->AddTendon(def + mjCOMPKIND_SHEAR);
      ten->def = model->defaults[0];
      ten->WrapSite(txt1);
      ten->WrapSite(txt2);

      // name tendon
      mju::sprintf_arr(txt, "%sTS%d_%d", prefix.c_str(), ix, iy);
      ten->name = txt;

      // equality constraint
      mjmEquality* eq = mjm_addEquality(model, def + mjCOMPKIND_SHEAR);
      mjm_setDefault(eq->element, model->defaults[0]);
      eq->type = mjEQ_TENDON;
      mjm_setString(eq->name1, txt);
    }
  }
}



// add skin to 2D
void mjCComposite::MakeSkin2(mjCModel* model, mjtNum inflate) {
  char txt[100];
  int N = count[0]*count[1];

  // add skin, set name and material
  mjCSkin* skin = model->AddSkin();
  mju::sprintf_arr(txt, "%sSkin", prefix.c_str());
  skin->name = txt;
  skin->set_material(skinmaterial);
  mjuu_copyvec(skin->rgba, skinrgba, 4);
  skin->inflate = inflate;
  skin->group = skingroup;

  // copy skin from existing mesh
  if (type==mjCOMPTYPE_PARTICLE && username.empty()) {
    std::vector<int> face;
    mjXUtil::String2Vector(userface, face);
    int nvert = uservert.size()/3;

    for (int j=0; j<2; j++) {
      for (int i=0; i<nvert; i++) {
        skin->vert.push_back(0);
        skin->vert.push_back(0);
        skin->vert.push_back(0);

        mju::sprintf_arr(txt, "%sB%d", prefix.c_str(), i);
        skin->bodyname.push_back(txt);
        skin->bindpos.push_back(0);
        skin->bindpos.push_back(0);
        skin->bindpos.push_back(0);
        skin->bindquat.push_back(1);
        skin->bindquat.push_back(0);
        skin->bindquat.push_back(0);
        skin->bindquat.push_back(0);

        vector<int> vertid;
        vector<float> vertweight;
        vertid.push_back(j*nvert+i);
        vertweight.push_back(1);
        skin->vertid.push_back(vertid);
        skin->vertweight.push_back(vertweight);
      }

      for (int i=0; i<face.size()/3; i++) {
        skin->face.push_back(j*nvert+face[3*i]);
        skin->face.push_back(j*nvert+face[3*i+(j==0 ? 1 : 2)]);
        skin->face.push_back(j*nvert+face[3*i+(j==0 ? 2 : 1)]);
      }
    }

    return;
  }

  // populate mesh: two sides
  for (int i=0; i<2; i++) {
    for (int ix=0; ix<count[0]; ix++) {
      for (int iy=0; iy<count[1]; iy++) {
        // vertex
        skin->vert.push_back(0);
        skin->vert.push_back(0);
        skin->vert.push_back(0);

        // texture coordinate
        if (skintexcoord) {
          skin->texcoord.push_back(ix/(float)(count[0]-1));
          skin->texcoord.push_back(iy/(float)(count[1]-1));
        }

        // face
        if (ix<count[0]-1 && iy<count[1]-1) {
          skin->face.push_back(i*N + ix*count[1]+iy);
          skin->face.push_back(i*N + (ix+1)*count[1]+iy+(i==1));
          skin->face.push_back(i*N + (ix+1)*count[1]+iy+(i==0));

          skin->face.push_back(i*N + ix*count[1]+iy);
          skin->face.push_back(i*N + (ix+(i==0))*count[1]+iy+1);
          skin->face.push_back(i*N + (ix+(i==1))*count[1]+iy+1);
        }
      }
    }
  }

  // add thin triangles: X direction, iy = 0
  for (int ix=0; ix<count[0]-1; ix++) {
    skin->face.push_back(ix*count[1]);
    skin->face.push_back(N + (ix+1)*count[1]);
    skin->face.push_back((ix+1)*count[1]);

    skin->face.push_back(ix*count[1]);
    skin->face.push_back(N + ix*count[1]);
    skin->face.push_back(N + (ix+1)*count[1]);
  }

  // add thin triangles: X direction, iy = count[1]-1
  for (int ix=0; ix<count[0]-1; ix++) {
    skin->face.push_back(ix*count[1] + count[1]-1);
    skin->face.push_back((ix+1)*count[1] + count[1]-1);
    skin->face.push_back(N + (ix+1)*count[1] + count[1]-1);

    skin->face.push_back(ix*count[1] + count[1]-1);
    skin->face.push_back(N + (ix+1)*count[1] + count[1]-1);
    skin->face.push_back(N + ix*count[1] + count[1]-1);
  }

  // add thin triangles: Y direction, ix = 0
  for (int iy=0; iy<count[1]-1; iy++) {
    skin->face.push_back(iy);
    skin->face.push_back(iy+1);
    skin->face.push_back(N + iy+1);

    skin->face.push_back(iy);
    skin->face.push_back(N + iy+1);
    skin->face.push_back(N + iy);
  }

  // add thin triangles: Y direction, ix = count[0]-1
  for (int iy=0; iy<count[1]-1; iy++) {
    skin->face.push_back(iy + (count[0]-1)*count[1]);
    skin->face.push_back(N + iy+1 + (count[0]-1)*count[1]);
    skin->face.push_back(iy+1 + (count[0]-1)*count[1]);

    skin->face.push_back(iy + (count[0]-1)*count[1]);
    skin->face.push_back(N + iy + (count[0]-1)*count[1]);
    skin->face.push_back(N + iy+1 + (count[0]-1)*count[1]);
  }

  // couple with bones
  if (type==mjCOMPTYPE_PARTICLE || type==mjCOMPTYPE_GRID) {
    MakeClothBones(model, skin);
  } else if (type==mjCOMPTYPE_CABLE) {
    MakeCableBones(model, skin);
  }
}



// add bones in 2D
void mjCComposite::MakeClothBones(mjCModel* model, mjCSkin* skin) {
  char txt[100];
  int N = count[0]*count[1];

  // populate bones
  for (int ix=0; ix<count[0]; ix++) {
    for (int iy=0; iy<count[1]; iy++) {
      // body name
      if (type==mjCOMPTYPE_GRID) {
        mju::sprintf_arr(txt, "%sB%d_%d", prefix.c_str(), ix, iy);
      } else {
        mju::sprintf_arr(txt, "%sB%d_%d_0", prefix.c_str(), ix, iy);
      }

      // bind pose
      skin->bodyname.push_back(txt);
      skin->bindpos.push_back(0);
      skin->bindpos.push_back(0);
      skin->bindpos.push_back(0);
      skin->bindquat.push_back(1);
      skin->bindquat.push_back(0);
      skin->bindquat.push_back(0);
      skin->bindquat.push_back(0);

      // create vertid and vertweight
      vector<int> vertid;
      vector<float> vertweight;
      vertid.push_back(ix*count[1]+iy);
      vertid.push_back(N + ix*count[1]+iy);
      vertweight.push_back(1);
      vertweight.push_back(1);
      skin->vertid.push_back(vertid);
      skin->vertweight.push_back(vertweight);
    }
  }
}



void mjCComposite::MakeClothBonesSubgrid(mjCModel* model, mjCSkin* skin) {
  char txt[100];

  // populate bones
  for (int ix=0; ix<count[0]; ix++) {
    for (int iy=0; iy<count[1]; iy++) {
      // body name
      if (type==mjCOMPTYPE_GRID) {
        mju::sprintf_arr(txt, "%sB%d_%d", prefix.c_str(), ix, iy);
      } else {
        mju::sprintf_arr(txt, "%sB%d_%d_0", prefix.c_str(), ix, iy);
      }

      // bind pose
      skin->bodyname.push_back(txt);
      skin->bindpos.push_back(ix*spacing);
      skin->bindpos.push_back(iy*spacing);
      skin->bindpos.push_back(0);
      skin->bindquat.push_back(1);
      skin->bindquat.push_back(0);
      skin->bindquat.push_back(0);
      skin->bindquat.push_back(0);

      // empty vertid and vertweight
      vector<int> vertid;
      vector<float> vertweight;
      skin->vertid.push_back(vertid);
      skin->vertweight.push_back(vertweight);
    }
  }
}



// add bones to 1D
void mjCComposite::MakeCableBones(mjCModel* model, mjCSkin* skin) {
  char this_body[100];
  int N = count[0]*count[1];

  // populate bones
  for (int ix=0; ix<count[0]; ix++) {
    for (int iy=0; iy<count[1]; iy++) {
      // body name
      if (ix==0) {
        mju::sprintf_arr(this_body, "%sB_first", prefix.c_str());
      } else if (ix>=count[0]-2) {
        mju::sprintf_arr(this_body, "%sB_last", prefix.c_str());
      } else {
        mju::sprintf_arr(this_body, "%sB_%d", prefix.c_str(), ix);
      }

      // bind pose
      if (iy==0) {
        skin->bodyname.push_back(this_body);
        skin->bindpos.push_back((ix==count[0]-1) ? -2*def[0].geom.spec.size[0] : 0);
        skin->bindpos.push_back(-def[0].geom.spec.size[1]);
        skin->bindpos.push_back(0);
        skin->bindquat.push_back(1); skin->bindquat.push_back(0);
        skin->bindquat.push_back(0); skin->bindquat.push_back(0);
      } else {
        skin->bodyname.push_back(this_body);
        skin->bindpos.push_back((ix==count[0]-1) ? -2*def[0].geom.spec.size[0] : 0);
        skin->bindpos.push_back(def[0].geom.spec.size[1]);
        skin->bindpos.push_back(0);
        skin->bindquat.push_back(1); skin->bindquat.push_back(0);
        skin->bindquat.push_back(0); skin->bindquat.push_back(0);
      }

      // create vertid and vertweight
      skin->vertid.push_back({ix*count[1]+iy, N + ix*count[1]+iy});
      skin->vertweight.push_back({1, 1});
    }
  }
}



void mjCComposite::MakeCableBonesSubgrid(mjCModel* model, mjCSkin* skin) {
  // populate bones
  for (int ix=0; ix<count[0]; ix++) {
    for (int iy=0; iy<count[1]; iy++) {
      char txt[100];

      // body name
      if (ix==0) {
        mju::sprintf_arr(txt, "%sB_first", prefix.c_str());
      } else if (ix>=count[0]-2) {
        mju::sprintf_arr(txt, "%sB_last", prefix.c_str());
      } else {
        mju::sprintf_arr(txt, "%sB_%d", prefix.c_str(), ix);
      }

      // bind pose
      if (iy==0) {
        skin->bindpos.push_back((ix==count[0]-1) ? -2*def[0].geom.spec.size[0] : 0);
        skin->bindpos.push_back(-def[0].geom.spec.size[1]);
        skin->bindpos.push_back(0);
      } else if (iy==2) {
        skin->bindpos.push_back((ix==count[0]-1) ? -2*def[0].geom.spec.size[0] : 0);
        skin->bindpos.push_back(def[0].geom.spec.size[1]);
        skin->bindpos.push_back(0);
      } else {
        skin->bindpos.push_back((ix==count[0]-1) ? -2*def[0].geom.spec.size[0] : 0);
        skin->bindpos.push_back(0);
        skin->bindpos.push_back(0);
      }
      skin->bodyname.push_back(txt);
      skin->bindquat.push_back(1);
      skin->bindquat.push_back(0);
      skin->bindquat.push_back(0);
      skin->bindquat.push_back(0);

      // empty vertid and vertweight
      skin->vertid.push_back({});
      skin->vertweight.push_back({});
    }
  }
}



//------------------------------------- subgrid matrices

// C = W * [f; f_x; f_y; f_xy]
static const mjtNum subW[16*16] = {
  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,
 -3,  0,  0,  3,  0,  0,  0,  0, -2,  0,  0, -1,  0,  0,  0,  0,
  2,  0,  0, -2,  0,  0,  0,  0,  1,  0,  0,  1,  0,  0,  0,  0,
  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,
  0,  0,  0,  0, -3,  0,  0,  3,  0,  0,  0,  0, -2,  0,  0, -1,
  0,  0,  0,  0,  2,  0,  0, -2,  0,  0,  0,  0,  1,  0,  0,  1,
 -3,  3,  0,  0, -2, -1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0, -3,  3,  0,  0, -2, -1,  0,  0,
  9, -9,  9, -9,  6,  3, -3, -6,  6, -6, -3,  3,  4,  2,  1,  2,
 -6,  6, -6,  6, -4, -2,  2,  4, -3,  3,  3, -3, -2, -1, -1, -2,
  2, -2,  0,  0,  1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  2, -2,  0,  0,  1,  1,  0,  0,
 -6,  6, -6,  6, -3, -3,  3,  3, -4,  4,  2, -2, -2, -2, -1, -1,
  4, -4,  4, -4,  2,  2, -2, -2,  2, -2, -2,  2,  1,  1,  1,  1
};

// left-bottom
static const mjtNum subD00[] = {
  5,  1, -1,                  // f
  9,  1, -1,
  10, 1, -1,
  6,  1, -1,

  5, -1,   9,  1,   -1,       // f_x
  5, -0.5, 13, 0.5, -1,
  6, -0.5, 14, 0.5, -1,
  6, -1,   10, 1,   -1,

  5, -1,   6,  1,   -1,       // f_y
  9, -1,   10, 1,   -1,
  9, -0.5, 11, 0.5, -1,
  5, -0.5, 7,  0.5, -1,

  9,  -1,    6, -1,    5, 1,    10, 1,    -1,     // f_xy
  13, -0.5,  6, -0.5,  5, 0.5,  14, 0.5,  -1,
  13, -0.25, 7, -0.25, 5, 0.25, 15, 0.25, -1,
  9,  -0.5,  7, -0.5,  5, 0.5,  11, 0.5,  -1
};

// center-bottom
static const mjtNum subD10[] = {
  5,  1, -1,                  // f
  9,  1, -1,
  10, 1, -1,
  6,  1, -1,

  1, -0.5, 9,  0.5, -1,       // f_x
  5, -0.5, 13, 0.5, -1,
  6, -0.5, 14, 0.5, -1,
  2, -0.5, 10, 0.5, -1,

  5, -1,   6,  1,   -1,       // f_y
  9, -1,   10, 1,   -1,
  9, -0.5, 11, 0.5, -1,
  5, -0.5, 7,  0.5, -1,

  9,  -0.5,  2, -0.5,  1, 0.5,  10, 0.5,  -1,     // f_xy
  13, -0.5,  6, -0.5,  5, 0.5,  14, 0.5,  -1,
  13, -0.25, 7, -0.25, 5, 0.25, 15, 0.25, -1,
  9,  -0.25, 3, -0.25, 1, 0.25, 11, 0.25, -1
};

// right-bottom
static const mjtNum subD20[] = {
  5,  1, -1,                  // f
  9,  1, -1,
  10, 1, -1,
  6,  1, -1,

  1, -0.5, 9,  0.5, -1,       // f_x
  5, -1,   9,  1,   -1,
  6, -1,   10, 1,   -1,
  2, -0.5, 10, 0.5, -1,

  5, -1,   6,  1,   -1,       // f_y
  9, -1,   10, 1,   -1,
  9, -0.5, 11, 0.5, -1,
  5, -0.5, 7,  0.5, -1,

  9, -0.5,  2, -0.5,  1, 0.5,  10, 0.5,  -1,      // f_xy
  9, -1,    6, -1,    5, 1,    10, 1,    -1,
  9, -0.5,  7, -0.5,  5, 0.5,  11, 0.5,  -1,
  9, -0.25, 3, -0.25, 1, 0.25, 11, 0.25, -1
};

// left-center
static const mjtNum subD01[] = {
  5,  1, -1,                  // f
  9,  1, -1,
  10, 1, -1,
  6,  1, -1,

  5, -1,   9,  1,   -1,       // f_x
  5, -0.5, 13, 0.5, -1,
  6, -0.5, 14, 0.5, -1,
  6, -1,   10, 1,   -1,

  4, -0.5, 6,  0.5, -1,       // f_y
  8, -0.5, 10, 0.5, -1,
  9, -0.5, 11, 0.5, -1,
  5, -0.5, 7,  0.5, -1,

  8,  -0.5,  6, -0.5,  4, 0.5,  10, 0.5,  -1,     // f_xy
  12, -0.25, 6, -0.25, 4, 0.25, 14, 0.25, -1,
  13, -0.25, 7, -0.25, 5, 0.25, 15, 0.25, -1,
  9,  -0.5,  7, -0.5,  5, 0.5,  11, 0.5,  -1
};

// center-center
static const mjtNum subD11[] = {
  5,  1, -1,                  // f
  9,  1, -1,
  10, 1, -1,
  6,  1, -1,

  1, -0.5, 9,  0.5, -1,       // f_x
  5, -0.5, 13, 0.5, -1,
  6, -0.5, 14, 0.5, -1,
  2, -0.5, 10, 0.5, -1,

  4, -0.5, 6,  0.5, -1,       // f_y
  8, -0.5, 10, 0.5, -1,
  9, -0.5, 11, 0.5, -1,
  5, -0.5, 7,  0.5, -1,

  8,  -0.25, 2, -0.25, 0, 0.25, 10, 0.25, -1,     // f_xy
  12, -0.25, 6, -0.25, 4, 0.25, 14, 0.25, -1,
  13, -0.25, 7, -0.25, 5, 0.25, 15, 0.25, -1,
  9,  -0.25, 3, -0.25, 1, 0.25, 11, 0.25, -1
};

// right-center
static const mjtNum subD21[] = {
  5,  1, -1,                  // f
  9,  1, -1,
  10, 1, -1,
  6,  1, -1,

  1, -0.5, 9,  0.5, -1,       // f_x
  5, -1,   9,  1,   -1,
  6, -1,   10, 1,   -1,
  2, -0.5, 10, 0.5, -1,

  4, -0.5, 6,  0.5, -1,       // f_y
  8, -0.5, 10, 0.5, -1,
  9, -0.5, 11, 0.5, -1,
  5, -0.5, 7,  0.5, -1,

  8, -0.25, 2, -0.25, 0, 0.25, 10, 0.25, -1,      // f_xy
  8, -0.5,  6, -0.5,  4, 0.5,  10, 0.5,  -1,
  9, -0.5,  7, -0.5,  5, 0.5,  11, 0.5,  -1,
  9, -0.25, 3, -0.25, 1, 0.25, 11, 0.25, -1
};

// left-top
static const mjtNum subD02[] = {
  5,  1, -1,                  // f
  9,  1, -1,
  10, 1, -1,
  6,  1, -1,

  5, -1,   9,  1,   -1,       // f_x
  5, -0.5, 13, 0.5, -1,
  6, -0.5, 14, 0.5, -1,
  6, -1,   10, 1,   -1,

  4, -0.5, 6,  0.5, -1,       // f_y
  8, -0.5, 10, 0.5, -1,
  9, -1,   10, 1,   -1,
  5, -1,   6,  1,   -1,

  8,  -0.5,  6, -0.5,  4, 0.5,  10, 0.5,  -1,     // f_xy
  12, -0.25, 6, -0.25, 4, 0.25, 14, 0.25, -1,
  13, -0.5,  6, -0.5,  5, 0.5,  14, 0.5,  -1,
  9,  -1,    6, -1,    5, 1,    10, 1,    -1
};

// center-top
static const mjtNum subD12[] = {
  5,  1, -1,                  // f
  9,  1, -1,
  10, 1, -1,
  6,  1, -1,

  1, -0.5, 9,  0.5, -1,       // f_x
  5, -0.5, 13, 0.5, -1,
  6, -0.5, 14, 0.5, -1,
  2, -0.5, 10, 0.5, -1,

  4, -0.5, 6,  0.5, -1,       // f_y
  8, -0.5, 10, 0.5, -1,
  9, -1,   10, 1,   -1,
  5, -1,   6,  1,   -1,

  8,  -0.25, 2, -0.25, 0, 0.25, 10, 0.25, -1,     // f_xy
  12, -0.25, 6, -0.25, 4, 0.25, 14, 0.25, -1,
  13, -0.5,  6, -0.5,  5, 0.5,  14, 0.5,  -1,
  9,  -0.5,  2, -0.5,  1, 0.5,  10, 0.5,  -1
};

// right-top
static const mjtNum subD22[] = {
  5,  1, -1,                  // f
  9,  1, -1,
  10, 1, -1,
  6,  1, -1,

  1, -0.5, 9,  0.5, -1,       // f_x
  5, -1,   9,  1,   -1,
  6, -1,   10, 1,   -1,
  2, -0.5, 10, 0.5, -1,

  4, -0.5, 6,  0.5, -1,       // f_y
  8, -0.5, 10, 0.5, -1,
  9, -1,   10, 1,   -1,
  5, -1,   6,  1,   -1,

  8, -0.25, 2, -0.25, 0, 0.25, 10, 0.25, -1,      // f_xy
  8, -0.5,  6, -0.5,  4, 0.5,  10, 0.5,  -1,
  9, -1,    6, -1,    5, 1,    10, 1,    -1,
  9, -0.5,  2, -0.5,  1, 0.5,  10, 0.5,  -1
};


// add skin to 2D, with subgrid
void mjCComposite::MakeSkin2Subgrid(mjCModel* model, mjtNum inflate) {
  // assemble pointers to Dxx matrices
  const mjtNum* Dp[3][3] = {
    {subD00, subD01, subD02},
    {subD10, subD11, subD12},
    {subD20, subD21, subD22}
  };

  // allocate
  const int N = (2+skinsubgrid)*(2+skinsubgrid);
  mjtNum* XY = (mjtNum*) mju_malloc(N*16*sizeof(mjtNum));
  mjtNum* XY_W = (mjtNum*) mju_malloc(N*16*sizeof(mjtNum));
  mjtNum* Weight = (mjtNum*) mju_malloc(9*N*16*sizeof(mjtNum));
  mjtNum* D = (mjtNum*) mju_malloc(16*16*sizeof(mjtNum));

  // XY matrix
  const mjtNum step = 1.0/(1+skinsubgrid);
  int rxy = 0;
  for (int sx=0; sx<=1+skinsubgrid; sx++) {
    for (int sy=0; sy<=1+skinsubgrid; sy++) {
      // compute x, y
      mjtNum x = sx*step;
      mjtNum y = sy*step;

      // make XY-row
      XY[16*rxy + 0] =  1;
      XY[16*rxy + 1] =  y;
      XY[16*rxy + 2] =  y*y;
      XY[16*rxy + 3] =  y*y*y;

      XY[16*rxy + 4] =  x*1;
      XY[16*rxy + 5] =  x*y;
      XY[16*rxy + 6] =  x*y*y;
      XY[16*rxy + 7] =  x*y*y*y;

      XY[16*rxy + 8] =  x*x*1;
      XY[16*rxy + 9] =  x*x*y;
      XY[16*rxy + 10] = x*x*y*y;
      XY[16*rxy + 11] = x*x*y*y*y;

      XY[16*rxy + 12] = x*x*x*1;
      XY[16*rxy + 13] = x*x*x*y;
      XY[16*rxy + 14] = x*x*x*y*y;
      XY[16*rxy + 15] = x*x*x*y*y*y;

      // advance row
      rxy++;
    }
  }

  // XY_W = XY * W
  mju_mulMatMat(XY_W, XY, subW, N, 16, 16);

  // Weight matrices
  for (int dx=0; dx<3; dx++) {
    for (int dy=0; dy<3; dy++) {
      // make dense D
      mju_zero(D, 16*16);
      int cnt = 0;
      int r = 0, c;
      while (r<16) {
        // scan row
        while ((c = mju_round(Dp[dx][dy][cnt]))!=-1) {
          D[r*16+c] = Dp[dx][dy][cnt+1];
          cnt +=2;
        }

        // advance
        r++;
        cnt++;
      }

      // Weight(d) = XY * W * D(d)
      mju_mulMatMat(Weight + (dx*3+dy)*N*16, XY_W, D, N, 16, 16);
    }
  }

  // add skin, set name and material
  char txt[100];
  mjCSkin* skin = model->AddSkin();
  mju::sprintf_arr(txt, "%sSkin", prefix.c_str());
  skin->name = txt;
  skin->set_material(skinmaterial);
  mjuu_copyvec(skin->rgba, skinrgba, 4);
  skin->inflate = inflate;
  skin->group = skingroup;

  // populate mesh: two sides
  mjtNum S = spacing/(1+skinsubgrid);
  int C0 = count[0] + (count[0]-1)*skinsubgrid;
  int C1 = count[1] + (count[1]-1)*skinsubgrid;
  int NN = C0*C1;
  for (int i=0; i<2; i++) {
    for (int ix=0; ix<C0; ix++) {
      for (int iy=0; iy<C1; iy++) {
        // vertex
        skin->vert.push_back(ix*S);
        skin->vert.push_back(iy*S);
        skin->vert.push_back(0);

        // texture coordinate
        if (skintexcoord) {
          skin->texcoord.push_back(ix/(float)(C0-1));
          skin->texcoord.push_back(iy/(float)(C1-1));
        }

        // face
        if (ix<C0-1 && iy<C1-1) {
          skin->face.push_back(i*NN + ix*C1+iy);
          skin->face.push_back(i*NN + (ix+1)*C1+iy+(i==1));
          skin->face.push_back(i*NN + (ix+1)*C1+iy+(i==0));

          skin->face.push_back(i*NN + ix*C1+iy);
          skin->face.push_back(i*NN + (ix+(i==0))*C1+iy+1);
          skin->face.push_back(i*NN + (ix+(i==1))*C1+iy+1);
        }
      }
    }
  }

  // add thin triangles: X direction, iy = 0
  for (int ix=0; ix<C0-1; ix++) {
    skin->face.push_back(ix*C1);
    skin->face.push_back(NN + (ix+1)*C1);
    skin->face.push_back((ix+1)*C1);

    skin->face.push_back(ix*C1);
    skin->face.push_back(NN + ix*C1);
    skin->face.push_back(NN + (ix+1)*C1);
  }

  // add thin triangles: X direction, iy = C1-1
  for (int ix=0; ix<C0-1; ix++) {
    skin->face.push_back(ix*C1 + C1-1);
    skin->face.push_back((ix+1)*C1 + C1-1);
    skin->face.push_back(NN + (ix+1)*C1 + C1-1);

    skin->face.push_back(ix*C1 + C1-1);
    skin->face.push_back(NN + (ix+1)*C1 + C1-1);
    skin->face.push_back(NN + ix*C1 + C1-1);
  }

  // add thin triangles: Y direction, ix = 0
  for (int iy=0; iy<C1-1; iy++) {
    skin->face.push_back(iy);
    skin->face.push_back(iy+1);
    skin->face.push_back(NN + iy+1);

    skin->face.push_back(iy);
    skin->face.push_back(NN + iy+1);
    skin->face.push_back(NN + iy);
  }

  // add thin triangles: Y direction, ix = C0-1
  for (int iy=0; iy<C1-1; iy++) {
    skin->face.push_back(iy + (C0-1)*C1);
    skin->face.push_back(NN + iy+1 + (C0-1)*C1);
    skin->face.push_back(iy+1 + (C0-1)*C1);

    skin->face.push_back(iy + (C0-1)*C1);
    skin->face.push_back(NN + iy + (C0-1)*C1);
    skin->face.push_back(NN + iy+1 + (C0-1)*C1);
  }

  if (type==mjCOMPTYPE_PARTICLE || type==mjCOMPTYPE_GRID) {
    MakeClothBonesSubgrid(model, skin);
  } else if (type==mjCOMPTYPE_CABLE) {
    MakeCableBonesSubgrid(model, skin);
  }

  // bind vertices to bones: one big square at a time
  for (int ix=0; ix<count[0]-1; ix++) {
    for (int iy=0; iy<count[1]-1; iy++) {
      // determine d for Weight indexing
      int d = 3 * (ix==0 ? 0 : (ix==count[0]-2 ? 2 : 1)) +
              (iy==0 ? 0 : (iy==count[1]-2 ? 2 : 1));

      // precompute 16 bone indices for big square
      int boneid[16];
      int cnt = 0;
      for (int dx=-1; dx<3; dx++) {
        for (int dy=-1; dy<3; dy++) {
          boneid[cnt++] = (ix+dx)*count[1] + (iy+dy);
        }
      }

      // process subgrid, top-rigth owns last index
      for (int dx=0; dx<1+skinsubgrid+(ix==count[0]-2); dx++) {
        for (int dy=0; dy<1+skinsubgrid+(iy==count[1]-2); dy++) {
          // recover vertex id
          int vid = (ix*(1+skinsubgrid)+dx)*C1 + iy*(1+skinsubgrid)+dy;

          // determine row in Weight
          int n = dx*(2+skinsubgrid) + dy;

          // add vertex to 16 bones
          for (int bi=0; bi<16; bi++) {
            mjtNum w = Weight[d*N*16 + n*16 + bi];
            if (w) {
              skin->vertid[boneid[bi]].push_back(vid);
              skin->vertid[boneid[bi]].push_back(vid+NN);
              skin->vertweight[boneid[bi]].push_back((float)w);
              skin->vertweight[boneid[bi]].push_back((float)w);
            }
          }
        }
      }
    }
  }

  // free allocations
  mju_free(XY);
  mju_free(XY_W);
  mju_free(Weight);
  mju_free(D);
}



// add skin to 3D
void mjCComposite::MakeSkin3(mjCModel* model) {
  int vcnt = 0;
  std::map<string, int> vmap;
  char txt[100], cnt0[10], cnt1[10], cnt2[10];
  string fmt;

  // string counts
  mju::sprintf_arr(cnt0, "%d", count[0]-1);
  mju::sprintf_arr(cnt1, "%d", count[1]-1);
  mju::sprintf_arr(cnt2, "%d", count[2]-1);

  // add skin, set name and material
  mjCSkin* skin = model->AddSkin();
  mju::sprintf_arr(txt, "%sSkin", prefix.c_str());
  skin->name = txt;
  skin->set_material(skinmaterial);
  mjuu_copyvec(skin->rgba, skinrgba, 4);
  skin->inflate = skininflate;
  skin->group = skingroup;

  // box
  if (type==mjCOMPTYPE_BOX || type==mjCOMPTYPE_PARTICLE) {
    // z-faces
    MakeSkin3Box(skin, count[0], count[1], 1, vcnt, "%sB%d_%d_0");
    fmt = "%sB%d_%d_" + string(cnt2);
    MakeSkin3Box(skin, count[0], count[1], 0, vcnt, fmt.c_str());

    // y-faces
    MakeSkin3Box(skin, count[0], count[2], 0, vcnt, "%sB%d_0_%d");
    fmt = "%sB%d_" + string(cnt1) + "_%d";
    MakeSkin3Box(skin, count[0], count[2], 1, vcnt, fmt.c_str());

    // x-faces
    MakeSkin3Box(skin, count[1], count[2], 1, vcnt, "%sB0_%d_%d");
    fmt = "%sB" + string(cnt0) + "_%d_%d";
    MakeSkin3Box(skin, count[1], count[2], 0, vcnt, fmt.c_str());
  }

  // cylinder
  else if (type==mjCOMPTYPE_CYLINDER) {
    // generate vertices in map
    for (int ix=0; ix<count[0]; ix++) {
      for (int iy=0; iy<count[1]; iy++) {
        for (int iz=0; iz<count[2]; iz++) {
          bool xedge = (ix==0 || ix==count[0]-1);
          bool yedge = (iy==0 || iy==count[1]-1);
          if (xedge || yedge) {
            // body name
            mju::sprintf_arr(txt, "%sB%d_%d_%d", prefix.c_str(), ix, iy, iz);

            // add vertex
            skin->vert.push_back(0);
            skin->vert.push_back(0);
            skin->vert.push_back(0);

            // texture coordinate
            if (skintexcoord) {
              float X = 0, Y = 0;
              if (xedge) {
                X = iy/(float)(count[1]-1);
                Y = iz/(float)(count[2]-1);
              } else {
                X = ix/(float)(count[0]-1);
                Y = iz/(float)(count[2]-1);
              }

              skin->texcoord.push_back(X);
              skin->texcoord.push_back(Y);
            }

            // save vertex id in map
            vmap[txt] = vcnt;
            vcnt++;
          }
        }
      }
    }

    // y-faces
    MakeSkin3Smooth(skin, count[0], count[2], 0, vmap, "%sB%d_0_%d");
    fmt = "%sB%d_" + string(cnt1) + "_%d";
    MakeSkin3Smooth(skin, count[0], count[2], 1, vmap, fmt.c_str());

    // x-faces
    MakeSkin3Smooth(skin, count[1], count[2], 1, vmap, "%sB0_%d_%d");
    fmt = "%sB" + string(cnt0) + "_%d_%d";
    MakeSkin3Smooth(skin, count[1], count[2], 0, vmap, fmt.c_str());

    // z-faces, boxy-type
    MakeSkin3Box(skin, count[0], count[1], 1, vcnt, "%sB%d_%d_0");
    fmt = "%sB%d_%d_" + string(cnt2);
    MakeSkin3Box(skin, count[0], count[1], 0, vcnt, fmt.c_str());
  }

  // smooth
  else {
    // generate vertices in map
    for (int ix=0; ix<count[0]; ix++) {
      for (int iy=0; iy<count[1]; iy++) {
        for (int iz=0; iz<count[2]; iz++) {
          bool xedge = (ix==0 || ix==count[0]-1);
          bool yedge = (iy==0 || iy==count[1]-1);
          bool zedge = (iz==0 || iz==count[2]-1);
          if (xedge || yedge || zedge) {
            // body name
            mju::sprintf_arr(txt, "%sB%d_%d_%d", prefix.c_str(), ix, iy, iz);

            // add vertex
            skin->vert.push_back(0);
            skin->vert.push_back(0);
            skin->vert.push_back(0);

            // texture coordinate
            if (skintexcoord) {
              float X = 0, Y = 0;
              if (xedge) {
                X = iy/(float)(count[1]-1);
                Y = iz/(float)(count[2]-1);
              } else if (yedge) {
                X = ix/(float)(count[0]-1);
                Y = iz/(float)(count[2]-1);
              } else {
                X = ix/(float)(count[0]-1);
                Y = iy/(float)(count[1]-1);
              }

              skin->texcoord.push_back(X);
              skin->texcoord.push_back(Y);
            }

            // save vertex id in map
            vmap[txt] = vcnt;
            vcnt++;
          }
        }
      }
    }

    // z-faces
    MakeSkin3Smooth(skin, count[0], count[1], 1, vmap, "%sB%d_%d_0");
    fmt = "%sB%d_%d_" + string(cnt2);
    MakeSkin3Smooth(skin, count[0], count[1], 0, vmap, fmt.c_str());

    // y-faces
    MakeSkin3Smooth(skin, count[0], count[2], 0, vmap, "%sB%d_0_%d");
    fmt = "%sB%d_" + string(cnt1) + "_%d";
    MakeSkin3Smooth(skin, count[0], count[2], 1, vmap, fmt.c_str());

    // x-faces
    MakeSkin3Smooth(skin, count[1], count[2], 1, vmap, "%sB0_%d_%d");
    fmt = "%sB" + string(cnt0) + "_%d_%d";
    MakeSkin3Smooth(skin, count[1], count[2], 0, vmap, fmt.c_str());
  }
}



// make one face of 3D skin, box
void mjCComposite::MakeSkin3Box(mjCSkin* skin, int c0, int c1, int side,
                                int& vcnt, const char* format) {
  char txt[100];

  // loop over bodies/vertices of specified face
  for (int i0=0; i0<c0; i0++) {
    for (int i1=0; i1<c1; i1++) {
      // vertex
      skin->vert.push_back(0);
      skin->vert.push_back(0);
      skin->vert.push_back(0);

      // texture coordinate
      if (skintexcoord) {
        skin->texcoord.push_back(i0/(float)(c0-1));
        skin->texcoord.push_back(i1/(float)(c1-1));
      }

      // face
      if (i0<c0-1 && i1<c1-1) {
        skin->face.push_back(vcnt + i0*c1+i1);
        skin->face.push_back(vcnt + (i0+1)*c1+i1+(side==1));
        skin->face.push_back(vcnt + (i0+1)*c1+i1+(side==0));

        skin->face.push_back(vcnt + i0*c1+i1);
        skin->face.push_back(vcnt + (i0+(side==0))*c1+i1+1);
        skin->face.push_back(vcnt + (i0+(side==1))*c1+i1+1);
      }

      // body name
      mju::sprintf_arr(txt, format, prefix.c_str(), i0, i1);

      // bind pose: origin
      skin->bodyname.push_back(txt);
      skin->bindpos.push_back(0);
      skin->bindpos.push_back(0);
      skin->bindpos.push_back(0);
      skin->bindquat.push_back(1);
      skin->bindquat.push_back(0);
      skin->bindquat.push_back(0);
      skin->bindquat.push_back(0);

      // vertid and vertweight
      vector<int> vertid;
      vector<float> vertweight;
      vertid.push_back(vcnt + i0*c1+i1);
      vertweight.push_back(1);
      skin->vertid.push_back(vertid);
      skin->vertweight.push_back(vertweight);
    }
  }

  // update vertex count
  vcnt += c0*c1;
}



// make one face of 3D skin, smooth
void mjCComposite::MakeSkin3Smooth(mjCSkin* skin, int c0, int c1, int side,
                                   const std::map<string, int>& vmap, const char* format) {
  char txt00[100], txt01[100], txt10[100], txt11[100];

  // loop over bodies/vertices of specified face
  for (int i0=0; i0<c0; i0++) {
    for (int i1=0; i1<c1; i1++) {
      // body names
      mju::sprintf_arr(txt00, format, prefix.c_str(), i0, i1);
      mju::sprintf_arr(txt01, format, prefix.c_str(), i0, i1+1);
      mju::sprintf_arr(txt10, format, prefix.c_str(), i0+1, i1);
      mju::sprintf_arr(txt11, format, prefix.c_str(), i0+1, i1+1);

      // face
      if (i0<c0-1 && i1<c1-1) {
        if (side==0) {
          skin->face.push_back(vmap.find(txt00)->second);
          skin->face.push_back(vmap.find(txt10)->second);
          skin->face.push_back(vmap.find(txt11)->second);

          skin->face.push_back(vmap.find(txt00)->second);
          skin->face.push_back(vmap.find(txt11)->second);
          skin->face.push_back(vmap.find(txt01)->second);
        } else {
          skin->face.push_back(vmap.find(txt00)->second);
          skin->face.push_back(vmap.find(txt01)->second);
          skin->face.push_back(vmap.find(txt11)->second);

          skin->face.push_back(vmap.find(txt00)->second);
          skin->face.push_back(vmap.find(txt11)->second);
          skin->face.push_back(vmap.find(txt10)->second);
        }
      }

      // bind pose: origin
      skin->bodyname.push_back(txt00);
      skin->bindpos.push_back(0);
      skin->bindpos.push_back(0);
      skin->bindpos.push_back(0);
      skin->bindquat.push_back(1);
      skin->bindquat.push_back(0);
      skin->bindquat.push_back(0);
      skin->bindquat.push_back(0);

      // vertid and vertweight
      vector<int> vertid;
      vector<float> vertweight;
      vertid.push_back(vmap.find(txt00)->second);
      vertweight.push_back(1);
      skin->vertid.push_back(vertid);
      skin->vertweight.push_back(vertweight);
    }
  }
}
