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

#include "xml/xml_native_reader.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tinyxml2.h"

#include <mujoco/mjmodel.h>
#include <mujoco/mjplugin.h>
#include <mujoco/mjvisualize.h>
#include <mujoco/mjtnum.h>
#include "engine/engine_plugin.h"
#include "engine/engine_util_errmem.h"
#include "engine/engine_util_misc.h"
#include "user/user_api.h"
#include "user/user_composite.h"
#include "user/user_flexcomp.h"
#include "user/user_model.h"
#include "user/user_objects.h"
#include "user/user_util.h"
#include "xml/xml_base.h"
#include "xml/xml_util.h"

namespace {
using std::string;
using std::vector;
using tinyxml2::XMLElement;

void ReadPluginConfigs(tinyxml2::XMLElement* elem, mjCPlugin* pp) {
  std::map<std::string, std::string, std::less<>> config_attribs;
  XMLElement* child = FirstChildElement(elem);
  while (child) {
    std::string_view name = child->Value();
    if (name == "config") {
      std::string key, value;
      mjXUtil::ReadAttrTxt(child, "key", key, /* required = */ true);
      if (config_attribs.find(key) != config_attribs.end()) {
        std::string err = "duplicate config key: " + key;
        throw mjXError(child, "%s", err.c_str());
      }
      mjXUtil::ReadAttrTxt(child, "value", value, /* required = */ true);
      config_attribs[key] = value;
    }
    child = NextSiblingElement(child);
  }

  if (!pp && !config_attribs.empty()) {
    throw mjXError(elem,
                   "plugin configuration attributes cannot be used in an "
                   "element that references a predefined plugin instance");
  } else if (pp) {
    pp->config_attribs = std::move(config_attribs);
  }
}
}  // namespace


//---------------------------------- MJCF schema ---------------------------------------------------

const char* MJCF[nMJCF][mjXATTRNUM] = {
{"mujoco", "!", "1", "model"},
{"<"},
    {"compiler", "*", "20", "autolimits", "boundmass", "boundinertia", "settotalmass",
        "balanceinertia", "strippath", "coordinate", "angle", "fitaabb", "eulerseq",
        "meshdir", "texturedir", "discardvisual", "convexhull", "usethread",
        "fusestatic", "inertiafromgeom", "inertiagrouprange", "exactmeshinertia",
        "assetdir"},
    {"<"},
        {"lengthrange", "?", "10", "mode", "useexisting", "uselimit",
            "accel", "maxforce", "timeconst", "timestep",
            "inttotal", "interval", "tolrange"},
    {">"},

    {"option", "*", "27",
        "timestep", "apirate", "impratio", "tolerance", "ls_tolerance", "noslip_tolerance",
        "mpr_tolerance", "gravity", "wind", "magnetic", "density", "viscosity",
        "o_margin", "o_solref", "o_solimp", "o_friction",
        "integrator", "cone", "jacobian",
        "solver", "iterations", "ls_iterations", "noslip_iterations", "mpr_iterations",
        "sdf_iterations", "sdf_initpoints", "actuatorgroupdisable"},
    {"<"},
        {"flag", "?", "22", "constraint", "equality", "frictionloss", "limit", "contact",
            "passive", "gravity", "clampctrl", "warmstart",
            "filterparent", "actuation", "refsafe", "sensor", "midphase", "eulerdamp",
            "override", "energy", "fwdinv", "invdiscrete", "sensornoise", "multiccd", "island"},
    {">"},

    {"size", "*", "14", "memory", "njmax", "nconmax", "nstack", "nuserdata", "nkey",
        "nuser_body", "nuser_jnt", "nuser_geom", "nuser_site", "nuser_cam",
        "nuser_tendon", "nuser_actuator", "nuser_sensor"},

    {"visual", "*", "0"},
    {"<"},
        {"global", "?", "11", "fovy", "ipd", "azimuth", "elevation", "linewidth", "glow",
            "offwidth", "offheight", "realtime", "ellipsoidinertia", "bvactive"},
        {"quality", "?", "5", "shadowsize", "offsamples", "numslices", "numstacks",
            "numquads"},
        {"headlight", "?", "4", "ambient", "diffuse", "specular", "active"},
        {"map", "?", "13", "stiffness", "stiffnessrot", "force", "torque", "alpha",
            "fogstart", "fogend", "znear", "zfar", "haze", "shadowclip", "shadowscale",
            "actuatortendon"},
        {"scale", "?", "17", "forcewidth", "contactwidth", "contactheight", "connect", "com",
            "camera", "light", "selectpoint", "jointlength", "jointwidth", "actuatorlength",
            "actuatorwidth", "framelength", "framewidth", "constraint", "slidercrank", "frustum"},
        {"rgba", "?", "25", "fog", "haze", "force", "inertia", "joint",
            "actuator", "actuatornegative", "actuatorpositive", "com",
            "camera", "light", "selectpoint", "connect", "contactpoint", "contactforce",
            "contactfriction", "contacttorque", "contactgap", "rangefinder",
            "constraint", "slidercrank", "crankbroken", "frustum", "bv", "bvactive"},
    {">"},

    {"statistic", "*", "5", "meaninertia", "meanmass", "meansize", "extent", "center"},

    {"default", "R", "1", "class"},
    {"<"},
        {"mesh", "?", "1", "scale"},
        {"material", "?", "8", "texture", "emission", "specular", "shininess",
            "reflectance", "rgba", "texrepeat", "texuniform"},
        {"joint", "?", "21", "type", "group", "pos", "axis", "springdamper",
            "limited", "actuatorfrclimited", "solreflimit", "solimplimit",
            "solreffriction", "solimpfriction", "stiffness", "range", "actuatorfrcrange",
            "margin", "ref", "springref", "armature", "damping", "frictionloss", "user"},
        {"geom", "?", "31", "type", "pos", "quat", "contype", "conaffinity", "condim",
            "group", "priority", "size", "material", "friction", "mass", "density",
            "shellinertia", "solmix", "solref", "solimp",
            "margin", "gap", "fromto", "axisangle", "xyaxes", "zaxis", "euler",
            "hfield", "mesh", "fitscale", "rgba", "fluidshape", "fluidcoef", "user"},
        {"site", "?", "13", "type", "group", "pos", "quat", "material",
            "size", "fromto", "axisangle", "xyaxes", "zaxis", "euler", "rgba", "user"},
        {"camera", "?", "16", "fovy", "ipd", "resolution", "pos", "quat", "axisangle", "xyaxes",
            "zaxis", "euler", "mode", "focal", "focalpixel", "principal", "principalpixel",
            "sensorsize", "user"},
        {"light", "?", "12", "pos", "dir", "directional", "castshadow", "active",
            "attenuation", "cutoff", "exponent", "ambient", "diffuse", "specular", "mode"},
        {"pair", "?", "7", "condim", "friction", "solref", "solreffriction", "solimp",
         "gap", "margin"},
        {"equality", "?", "3", "active", "solref", "solimp"},
        {"tendon", "?", "16", "group", "limited", "range",
            "solreflimit", "solimplimit", "solreffriction", "solimpfriction",
            "frictionloss", "springlength", "width", "material",
            "margin", "stiffness", "damping", "rgba", "user"},
        {"general", "?", "18", "ctrllimited", "forcelimited", "actlimited", "ctrlrange",
            "forcerange", "actrange", "gear", "cranklength", "user", "group", "actdim",
            "dyntype", "gaintype", "biastype", "dynprm", "gainprm", "biasprm", "actearly"},
        {"motor", "?", "8", "ctrllimited", "forcelimited", "ctrlrange", "forcerange",
            "gear", "cranklength", "user", "group"},
        {"position", "?", "10", "ctrllimited", "forcelimited", "ctrlrange", "forcerange",
            "gear", "cranklength", "user", "group",
            "kp", "kv"},
        {"velocity", "?", "9", "ctrllimited", "forcelimited", "ctrlrange", "forcerange",
            "gear", "cranklength", "user", "group",
            "kv"},
        {"intvelocity", "?", "11", "ctrllimited", "forcelimited",
            "ctrlrange", "forcerange", "actrange",
            "gear", "cranklength", "user", "group",
            "kp", "kv"},
        {"damper", "?", "8", "forcelimited", "ctrlrange", "forcerange",
            "gear", "cranklength", "user", "group",
            "kv"},
        {"cylinder", "?", "12", "ctrllimited", "forcelimited", "ctrlrange", "forcerange",
            "gear", "cranklength", "user", "group",
            "timeconst", "area", "diameter", "bias"},
        {"muscle", "?", "17", "ctrllimited", "forcelimited", "ctrlrange", "forcerange",
            "gear", "cranklength", "user", "group",
            "timeconst", "range", "force", "scale",
            "lmin", "lmax", "vmax", "fpmax", "fvmax"},
        {"adhesion", "?", "6", "forcelimited", "ctrlrange", "forcerange",
            "gain", "user", "group"},
    {">"},

    {"extension", "*", "0"},
    {"<"},
        {"plugin", "*", "1", "plugin"},
        {"<"},
            {"instance", "*", "1", "name"},
            {"<"},
                {"config", "*", "2", "key", "value"},
            {">"},
        {">"},
    {">"},

    {"custom", "*", "0"},
    {"<"},
        {"numeric", "*", "3", "name", "size", "data"},
        {"text", "*", "2", "name", "data"},
        {"tuple", "*", "1", "name"},
        {"<"},
            {"element", "*", "3", "objtype", "objname", "prm"},
        {">"},
    {">"},

    {"asset", "*", "0"},
    {"<"},
        {"texture", "*", "22", "name", "type", "content_type", "file", "gridsize", "gridlayout",
            "fileright", "fileleft", "fileup", "filedown", "filefront", "fileback",
            "builtin", "rgb1", "rgb2", "mark", "markrgb", "random", "width", "height",
            "hflip", "vflip"},
        {"hfield", "*", "7", "name", "content_type", "file", "nrow", "ncol", "size", "elevation"},
        {"mesh", "*", "12", "name", "class", "content_type", "file", "vertex", "normal",
            "texcoord", "face", "refpos", "refquat", "scale", "smoothnormal"},
        {"<"},
          {"plugin", "*", "2", "plugin", "instance"},
          {"<"},
            {"config", "*", "2", "key", "value"},
          {">"},
        {">"},
        {"skin", "*", "9", "name", "file", "material", "rgba", "inflate",
            "vertex", "texcoord", "face", "group"},
        {"<"},
            {"bone", "*", "5", "body", "bindpos", "bindquat", "vertid", "vertweight"},
        {">"},
        {"material", "*", "10", "name", "class", "texture",  "texrepeat", "texuniform",
            "emission", "specular", "shininess", "reflectance", "rgba"},
    {">"},

    {"body", "R", "11", "name", "childclass", "pos", "quat", "mocap",
        "axisangle", "xyaxes", "zaxis", "euler", "gravcomp", "user"},
    {"<"},
        {"inertial", "?", "9", "pos", "quat", "mass", "diaginertia",
            "axisangle", "xyaxes", "zaxis", "euler", "fullinertia"},
        {"joint", "*", "23", "name", "class", "type", "group", "pos", "axis",
            "springdamper", "limited", "actuatorfrclimited",
            "solreflimit", "solimplimit", "solreffriction", "solimpfriction",
            "stiffness", "range", "actuatorfrcrange", "margin", "ref", "springref",
            "armature", "damping", "frictionloss", "user"},
        {"freejoint", "*", "2", "name", "group"},
        {"geom", "*", "33", "name", "class", "type", "contype", "conaffinity", "condim",
            "group", "priority", "size", "material", "friction", "mass", "density",
            "shellinertia", "solmix", "solref", "solimp",
            "margin", "gap", "fromto", "pos", "quat", "axisangle", "xyaxes", "zaxis", "euler",
            "hfield", "mesh", "fitscale", "rgba", "fluidshape", "fluidcoef", "user"},
        {"<"},
            {"plugin", "*", "2", "plugin", "instance"},
            {"<"},
              {"config", "*", "2", "key", "value"},
            {">"},
        {">"},
        {"site", "*", "15", "name", "class", "type", "group", "pos", "quat",
            "material", "size", "fromto", "axisangle", "xyaxes", "zaxis", "euler", "rgba", "user"},
        {"camera", "*", "19", "name", "class", "fovy", "ipd", "resolution", "pos", "quat",
            "axisangle", "xyaxes", "zaxis", "euler", "mode", "target", "focal", "focalpixel",
            "principal", "principalpixel", "sensorsize", "user"},
        {"light", "*", "15", "name", "class", "directional", "castshadow", "active",
            "pos", "dir", "attenuation", "cutoff", "exponent", "ambient", "diffuse", "specular",
            "mode", "target"},
        {"plugin", "*", "2", "plugin", "instance"},
        {"<"},
          {"config", "*", "2", "key", "value"},
        {">"},
        {"composite", "*", "13", "prefix", "type", "count", "spacing", "offset",
            "flatinertia", "solrefsmooth", "solimpsmooth", "vertex", "face",
            "initial", "curve", "size"},
        {"<"},
            {"joint", "*", "17", "kind", "group", "stiffness", "damping", "armature",
                "solreffix", "solimpfix", "type", "axis",
                "limited", "range", "margin", "solreflimit", "solimplimit",
                "frictionloss", "solreffriction", "solimpfriction"},
            {"tendon", "*", "17", "kind", "group", "stiffness", "damping",
                "solreffix", "solimpfix",
                "limited", "range", "margin", "solreflimit", "solimplimit",
                "frictionloss", "solreffriction", "solimpfriction",
                "material", "rgba", "width"},
            {"skin", "?", "6", "texcoord", "material", "group", "rgba", "inflate", "subgrid"},
            {"geom", "?", "17", "type", "contype", "conaffinity", "condim",
                "group", "priority", "size", "material", "rgba", "friction", "mass",
                "density", "solmix", "solref", "solimp", "margin", "gap"},
            {"site", "?", "4", "group", "size", "material", "rgba"},
            {"pin", "*", "1", "coord"},
            {"plugin", "*", "2", "plugin", "instance"},
            {"<"},
              {"config", "*", "2", "key", "value"},
            {">"},
        {">"},
        {"flexcomp", "*", "24", "name", "type", "group", "dim",
            "count", "spacing", "radius", "rigid", "mass", "inertiabox",
            "scale", "file", "point", "element", "texcoord", "material", "rgba",
            "flatskin", "pos", "quat", "axisangle", "xyaxes", "zaxis", "euler"},
        {"<"},
            {"edge", "?", "5", "equality", "solref", "solimp", "stiffness", "damping"},
            {"contact", "?", "13", "contype", "conaffinity", "condim", "priority",
                "friction", "solmix", "solref", "solimp", "margin", "gap",
                "internal", "selfcollide", "activelayers"},
            {"pin", "*", "4", "id", "range", "grid", "gridrange"},
            {"plugin", "*", "2", "plugin", "instance"},
            {"<"},
              {"config", "*", "2", "key", "value"},
            {">"},
        {">"},
    {">"},

    {"deformable", "*", "0"},
    {"<"},
        {"flex", "*", "11", "name", "group", "dim", "radius", "material",
            "rgba", "flatskin", "body", "vertex", "element", "texcoord"},
        {"<"},
            {"contact", "?", "13", "contype", "conaffinity", "condim", "priority",
                "friction", "solmix", "solref", "solimp", "margin", "gap",
                "internal", "selfcollide", "activelayers"},
            {"edge", "?", "2", "stiffness", "damping"},
        {">"},
        {"skin", "*", "9", "name", "file", "material", "rgba", "inflate",
            "vertex", "texcoord", "face", "group"},
        {"<"},
            {"bone", "*", "5", "body", "bindpos", "bindquat", "vertid", "vertweight"},
        {">"},
    {">"},

    {"contact", "*", "0"},
    {"<"},
        {"pair", "*", "11", "name", "class", "geom1", "geom2", "condim", "friction",
            "solref", "solreffriction", "solimp", "gap", "margin"},
        {"exclude", "*", "3", "name", "body1", "body2"},
    {">"},

    {"equality", "*", "0"},
    {"<"},
        {"connect", "*", "8", "name", "class", "body1", "body2", "anchor",
            "active", "solref", "solimp"},
        {"weld", "*", "10", "name", "class", "body1", "body2", "relpose", "anchor",
            "active", "solref", "solimp", "torquescale"},
        {"joint", "*", "8", "name", "class", "joint1", "joint2", "polycoef",
            "active", "solref", "solimp"},
        {"tendon", "*", "8", "name", "class", "tendon1", "tendon2", "polycoef",
            "active", "solref", "solimp"},
        {"flex", "*", "6", "name", "class", "flex",
            "active", "solref", "solimp"},
    {">"},

    {"tendon", "*", "0"},
    {"<"},
        {"spatial", "*", "18", "name", "class", "group", "limited", "range",
            "solreflimit", "solimplimit", "solreffriction", "solimpfriction",
            "frictionloss", "springlength", "width", "material",
            "margin", "stiffness", "damping", "rgba", "user"},
        {"<"},
            {"site", "*", "1", "site"},
            {"geom", "*", "2", "geom", "sidesite"},
            {"pulley", "*", "1", "divisor"},
        {">"},
        {"fixed", "*", "15", "name", "class", "group", "limited", "range",
            "solreflimit", "solimplimit", "solreffriction", "solimpfriction",
            "frictionloss", "springlength", "margin", "stiffness", "damping", "user"},
        {"<"},
            {"joint", "*", "2", "joint", "coef"},
        {">"},
    {">"},

    {"actuator", "*", "0"},
    {"<"},
        {"general", "*", "29", "name", "class", "group",
            "ctrllimited", "forcelimited", "actlimited", "ctrlrange", "forcerange", "actrange",
            "lengthrange", "gear", "cranklength", "user",
            "joint", "jointinparent", "tendon", "slidersite", "cranksite", "site", "refsite",
            "body", "actdim", "dyntype", "gaintype", "biastype", "dynprm", "gainprm", "biasprm",
            "actearly"},
        {"motor", "*", "18", "name", "class", "group",
            "ctrllimited", "forcelimited", "ctrlrange", "forcerange",
            "lengthrange", "gear", "cranklength", "user",
            "joint", "jointinparent", "tendon", "slidersite", "cranksite", "site", "refsite"},
        {"position", "*", "20", "name", "class", "group",
            "ctrllimited", "forcelimited", "ctrlrange", "forcerange",
            "lengthrange", "gear", "cranklength", "user",
            "joint", "jointinparent", "tendon", "slidersite", "cranksite", "site", "refsite",
            "kp", "kv"},
        {"velocity", "*", "19", "name", "class", "group",
            "ctrllimited", "forcelimited", "ctrlrange", "forcerange",
            "lengthrange", "gear", "cranklength", "user",
            "joint", "jointinparent", "tendon", "slidersite", "cranksite", "site", "refsite",
            "kv"},
        {"intvelocity", "*", "21", "name", "class", "group",
            "ctrllimited", "forcelimited",
            "ctrlrange", "forcerange", "actrange", "lengthrange",
            "gear", "cranklength", "user",
            "joint", "jointinparent", "tendon", "slidersite", "cranksite", "site", "refsite",
            "kp", "kv"},
        {"damper", "*", "18", "name", "class", "group",
            "forcelimited", "ctrlrange", "forcerange",
            "lengthrange", "gear", "cranklength", "user",
            "joint", "jointinparent", "tendon", "slidersite", "cranksite", "site", "refsite",
            "kv"},
        {"cylinder", "*", "22", "name", "class", "group",
            "ctrllimited", "forcelimited", "ctrlrange", "forcerange",
            "lengthrange", "gear", "cranklength", "user",
            "joint", "jointinparent", "tendon", "slidersite", "cranksite", "site", "refsite",
            "timeconst", "area", "diameter", "bias"},
        {"muscle", "*", "26",  "name", "class", "group",
            "ctrllimited", "forcelimited", "ctrlrange", "forcerange",
            "lengthrange", "gear", "cranklength", "user",
            "joint", "jointinparent", "tendon", "slidersite", "cranksite",
            "timeconst", "tausmooth", "range", "force", "scale",
            "lmin", "lmax", "vmax", "fpmax", "fvmax"},
        {"adhesion", "*", "9", "name", "class", "group",
            "forcelimited", "ctrlrange", "forcerange", "user", "body", "gain"},
        {"plugin", "*", "24", "name", "class",  "plugin", "instance", "group",
            "ctrllimited", "forcelimited", "actlimited", "ctrlrange", "forcerange", "actrange",
            "lengthrange", "gear", "cranklength", "joint", "jointinparent",
            "site", "dyntype", "dynprm", "tendon", "cranksite", "slidersite", "user", "actearly"},
        {"<"},
          {"config", "*", "2", "key", "value"},
        {">"},
    {">"},

    {"sensor", "*", "0"},
    {"<"},
        {"touch", "*", "5", "name", "site", "cutoff", "noise", "user"},
        {"accelerometer", "*", "5", "name", "site", "cutoff", "noise", "user"},
        {"velocimeter", "*", "5", "name", "site", "cutoff", "noise", "user"},
        {"gyro", "*", "5", "name", "site", "cutoff", "noise", "user"},
        {"force", "*", "5", "name", "site", "cutoff", "noise", "user"},
        {"torque", "*", "5", "name", "site", "cutoff", "noise", "user"},
        {"magnetometer", "*", "5", "name", "site", "cutoff", "noise", "user"},
        {"camprojection", "*", "6", "name", "site", "camera", "cutoff", "noise", "user"},
        {"rangefinder", "*", "5", "name", "site", "cutoff", "noise", "user"},
        {"jointpos", "*", "5", "name", "joint", "cutoff", "noise", "user"},
        {"jointvel", "*", "5", "name", "joint", "cutoff", "noise", "user"},
        {"tendonpos", "*", "5", "name", "tendon", "cutoff", "noise", "user"},
        {"tendonvel", "*", "5", "name", "tendon", "cutoff", "noise", "user"},
        {"actuatorpos", "*", "5", "name", "actuator", "cutoff", "noise", "user"},
        {"actuatorvel", "*", "5", "name", "actuator", "cutoff", "noise", "user"},
        {"actuatorfrc", "*", "5", "name", "actuator", "cutoff", "noise", "user"},
        {"jointactuatorfrc", "*", "5", "name", "joint", "cutoff", "noise", "user"},
        {"ballquat", "*", "5", "name", "joint", "cutoff", "noise", "user"},
        {"ballangvel", "*", "5", "name", "joint", "cutoff", "noise", "user"},
        {"jointlimitpos", "*", "5", "name", "joint", "cutoff", "noise", "user"},
        {"jointlimitvel", "*", "5", "name", "joint", "cutoff", "noise", "user"},
        {"jointlimitfrc", "*", "5", "name", "joint", "cutoff", "noise", "user"},
        {"tendonlimitpos", "*", "5", "name", "tendon", "cutoff", "noise", "user"},
        {"tendonlimitvel", "*", "5", "name", "tendon", "cutoff", "noise", "user"},
        {"tendonlimitfrc", "*", "5", "name", "tendon", "cutoff", "noise", "user"},
        {"framepos", "*", "8", "name", "objtype", "objname", "reftype", "refname", "cutoff", "noise", "user"},
        {"framequat", "*", "8", "name", "objtype", "objname", "reftype", "refname", "cutoff", "noise", "user"},
        {"framexaxis", "*", "8", "name", "objtype", "objname", "reftype", "refname", "cutoff", "noise", "user"},
        {"frameyaxis", "*", "8", "name", "objtype", "objname", "reftype", "refname", "cutoff", "noise", "user"},
        {"framezaxis", "*", "8", "name", "objtype", "objname", "reftype", "refname", "cutoff", "noise", "user"},
        {"framelinvel", "*", "8", "name", "objtype", "objname", "reftype", "refname", "cutoff", "noise", "user"},
        {"frameangvel", "*", "8", "name", "objtype", "objname", "reftype", "refname", "cutoff", "noise", "user"},
        {"framelinacc", "*", "6", "name", "objtype", "objname", "cutoff", "noise", "user"},
        {"frameangacc", "*", "6", "name", "objtype", "objname", "cutoff", "noise", "user"},
        {"subtreecom", "*", "5", "name", "body", "cutoff", "noise", "user"},
        {"subtreelinvel", "*", "5", "name", "body", "cutoff", "noise", "user"},
        {"subtreeangmom", "*", "5", "name", "body", "cutoff", "noise", "user"},
        {"clock", "*", "4", "name", "cutoff", "noise", "user"},
        {"user", "*", "9", "name", "objtype", "objname", "datatype", "needstage",
            "dim", "cutoff", "noise", "user"},
        {"plugin", "*", "9", "name", "plugin", "instance", "cutoff", "objtype", "objname", "reftype", "refname",
            "user"},
        {"<"},
          {"config", "*", "2", "key", "value"},
        {">"},
    {">"},

    {"keyframe", "*", "0"},
    {"<"},
        {"key", "*", "8", "name", "time", "qpos", "qvel", "act", "mpos", "mquat", "ctrl"},
    {">"},
{">"}
};



//---------------------------------- MJCF keywords used in attributes ------------------------------

// coordinate type
const mjMap coordinate_map[2] = {
  {"local",   0},
  {"global",  1}
};


// angle type
const mjMap angle_map[2] = {
  {"radian",  0},
  {"degree",  1}
};


// bool type
const mjMap bool_map[2] = {
  {"false",   0},
  {"true",    1}
};


// fluidshape type
const mjMap fluid_map[2] = {
  {"none",      0},
  {"ellipsoid", 1}
};


// enable type
const mjMap enable_map[2] = {
  {"disable", 0},
  {"enable",  1}
};


// TFAuto type
const mjMap TFAuto_map[3] = {
  {"false",   0},
  {"true",    1},
  {"auto",    2}
};


// joint type
const int joint_sz = 4;
const mjMap joint_map[joint_sz] = {
  {"free",        mjJNT_FREE},
  {"ball",        mjJNT_BALL},
  {"slide",       mjJNT_SLIDE},
  {"hinge",       mjJNT_HINGE}
};


// geom type
const mjMap geom_map[mjNGEOMTYPES] = {
  {"plane",       mjGEOM_PLANE},
  {"hfield",      mjGEOM_HFIELD},
  {"sphere",      mjGEOM_SPHERE},
  {"capsule",     mjGEOM_CAPSULE},
  {"ellipsoid",   mjGEOM_ELLIPSOID},
  {"cylinder",    mjGEOM_CYLINDER},
  {"box",         mjGEOM_BOX},
  {"mesh",        mjGEOM_MESH},
  {"sdf",         mjGEOM_SDF}
};


// camlight type
const int camlight_sz = 5;
const mjMap camlight_map[camlight_sz] = {
  {"fixed",         mjCAMLIGHT_FIXED},
  {"track",         mjCAMLIGHT_TRACK},
  {"trackcom",      mjCAMLIGHT_TRACKCOM},
  {"targetbody",    mjCAMLIGHT_TARGETBODY},
  {"targetbodycom", mjCAMLIGHT_TARGETBODYCOM}
};


// integrator type
const int integrator_sz = 4;
const mjMap integrator_map[integrator_sz] = {
  {"Euler",        mjINT_EULER},
  {"RK4",          mjINT_RK4},
  {"implicit",     mjINT_IMPLICIT},
  {"implicitfast", mjINT_IMPLICITFAST}
};

// cone type
const int cone_sz = 2;
const mjMap cone_map[cone_sz] = {
  {"pyramidal",   mjCONE_PYRAMIDAL},
  {"elliptic",    mjCONE_ELLIPTIC}
};


// Jacobian type
const int jac_sz = 3;
const mjMap jac_map[jac_sz] = {
  {"dense",       mjJAC_DENSE},
  {"sparse",      mjJAC_SPARSE},
  {"auto",        mjJAC_AUTO}
};


// solver type
const int solver_sz = 3;
const mjMap solver_map[solver_sz] = {
  {"PGS",         mjSOL_PGS},
  {"CG",          mjSOL_CG},
  {"Newton",      mjSOL_NEWTON}
};


// constraint type
const int equality_sz = 6;
const mjMap equality_map[equality_sz] = {
  {"connect",     mjEQ_CONNECT},
  {"weld",        mjEQ_WELD},
  {"joint",       mjEQ_JOINT},
  {"tendon",      mjEQ_TENDON},
  {"flex",        mjEQ_FLEX},
  {"distance",    mjEQ_DISTANCE}
};


// type for texture
const int texture_sz = 3;
const mjMap texture_map[texture_sz] = {
  {"2d",          mjTEXTURE_2D},
  {"cube",        mjTEXTURE_CUBE},
  {"skybox",      mjTEXTURE_SKYBOX}
};


// builtin type for texture
const int builtin_sz = 4;
const mjMap builtin_map[builtin_sz] = {
  {"none",        mjBUILTIN_NONE},
  {"gradient",    mjBUILTIN_GRADIENT},
  {"checker",     mjBUILTIN_CHECKER},
  {"flat",        mjBUILTIN_FLAT}
};


// mark type for texture
const int mark_sz = 4;
const mjMap mark_map[mark_sz] = {
  {"none",        mjMARK_NONE},
  {"edge",        mjMARK_EDGE},
  {"cross",       mjMARK_CROSS},
  {"random",      mjMARK_RANDOM}
};


// dyn type
const int dyn_sz = 6;
const mjMap dyn_map[dyn_sz] = {
  {"none",        mjDYN_NONE},
  {"integrator",  mjDYN_INTEGRATOR},
  {"filter",      mjDYN_FILTER},
  {"filterexact", mjDYN_FILTEREXACT},
  {"muscle",      mjDYN_MUSCLE},
  {"user",        mjDYN_USER}
};


// gain type
const int gain_sz = 4;
const mjMap gain_map[gain_sz] = {
  {"fixed",       mjGAIN_FIXED},
  {"affine",      mjGAIN_AFFINE},
  {"muscle",      mjGAIN_MUSCLE},
  {"user",        mjGAIN_USER}
};


// bias type
const int bias_sz = 4;
const mjMap bias_map[bias_sz] = {
  {"none",        mjBIAS_NONE},
  {"affine",      mjBIAS_AFFINE},
  {"muscle",      mjBIAS_MUSCLE},
  {"user",        mjBIAS_USER}
};


// stage type
const int stage_sz = 4;
const mjMap stage_map[stage_sz] = {
  {"none",        mjSTAGE_NONE},
  {"pos",         mjSTAGE_POS},
  {"vel",         mjSTAGE_VEL},
  {"acc",         mjSTAGE_ACC}
};


// data type
const int datatype_sz = 4;
const mjMap datatype_map[datatype_sz] = {
  {"real",        mjDATATYPE_REAL},
  {"positive",    mjDATATYPE_POSITIVE},
  {"axis",        mjDATATYPE_AXIS},
  {"quaternion",  mjDATATYPE_QUATERNION}
};


// LR mode
const int lrmode_sz = 4;
const mjMap lrmode_map[lrmode_sz] = {
  {"none",        mjLRMODE_NONE},
  {"muscle",      mjLRMODE_MUSCLE},
  {"muscleuser",  mjLRMODE_MUSCLEUSER},
  {"all",         mjLRMODE_ALL}
};


// composite type
const mjMap comp_map[mjNCOMPTYPES] = {
  {"particle",    mjCOMPTYPE_PARTICLE},
  {"grid",        mjCOMPTYPE_GRID},
  {"rope",        mjCOMPTYPE_ROPE},
  {"loop",        mjCOMPTYPE_LOOP},
  {"cable",       mjCOMPTYPE_CABLE},
  {"cloth",       mjCOMPTYPE_CLOTH},
  {"box",         mjCOMPTYPE_BOX},
  {"cylinder",    mjCOMPTYPE_CYLINDER},
  {"ellipsoid",   mjCOMPTYPE_ELLIPSOID}
};


// composite joint kind
const mjMap jkind_map[4] = {
  {"main",        mjCOMPKIND_JOINT},
  {"twist",       mjCOMPKIND_TWIST},
  {"stretch",     mjCOMPKIND_STRETCH},
  {"particle",    mjCOMPKIND_PARTICLE}
};


// composite rope shape
const mjMap shape_map[mjNCOMPSHAPES] = {
  {"s",           mjCOMPSHAPE_LINE},
  {"cos(s)",      mjCOMPSHAPE_COS},
  {"sin(s)",      mjCOMPSHAPE_SIN},
  {"0",           mjCOMPSHAPE_ZERO}
};


// composite tendon kind
const mjMap tkind_map[2] = {
  {"main",        mjCOMPKIND_TENDON},
  {"shear",       mjCOMPKIND_SHEAR}
};


// mesh type
const mjMap meshtype_map[2] = {
  {"false", mjINERTIA_VOLUME},
  {"true",  mjINERTIA_SHELL},
};


// flexcomp type
const mjMap fcomp_map[mjNFCOMPTYPES] = {
  {"grid",        mjFCOMPTYPE_GRID},
  {"box",         mjFCOMPTYPE_BOX},
  {"cylinder",    mjFCOMPTYPE_CYLINDER},
  {"ellipsoid",   mjFCOMPTYPE_ELLIPSOID},
  {"mesh",        mjFCOMPTYPE_MESH},
  {"gmsh",        mjFCOMPTYPE_GMSH},
  {"direct",      mjFCOMPTYPE_DIRECT}
};


// flex selfcollide type
const mjMap flexself_map[5] = {
  {"none",        mjFLEXSELF_NONE},
  {"narrow",      mjFLEXSELF_NARROW},
  {"bvh",         mjFLEXSELF_BVH},
  {"sap",         mjFLEXSELF_SAP},
  {"auto",        mjFLEXSELF_AUTO},
};



//---------------------------------- class mjXReader implementation --------------------------------

// constructor
mjXReader::mjXReader() : schema(MJCF, nMJCF) {
  readingdefaults = false;
}



// print schema
void mjXReader::PrintSchema(std::stringstream& str, bool html, bool pad) {
  if (html) {
    schema.PrintHTML(str, 0, pad);
  } else {
    schema.Print(str, 0);
  }
}



// main entry point for XML parser
//  mjCModel is allocated here; caller is responsible for deallocation
void mjXReader::Parse(XMLElement* root) {
  // check schema
  if (!schema.GetError().empty()) {
    throw mjXError(0, "XML Schema Construction Error: %s\n",
                   schema.GetError().c_str());
  }

  // validate
  XMLElement* bad = 0;
  if ((bad = schema.Check(root, 0))) {
    throw mjXError(bad, "Schema violation: %s\n",
                   schema.GetError().c_str());
  }

  // get model name
  ReadAttrTxt(root, "model", model->modelname);

  // get comment
  if (root->FirstChild() && root->FirstChild()->ToComment()) {
    model->comment = root->FirstChild()->Value();
  } else {
    model->comment.clear();
  }

  //------------------- parse MuJoCo sections embedded in all XML formats

  for (XMLElement* section = FirstChildElement(root, "compiler"); section;
       section = NextSiblingElement(section, "compiler")) {
    Compiler(section, model);
  }

  for (XMLElement* section = FirstChildElement(root, "option"); section;
       section = NextSiblingElement(section, "option")) {
    Option(section, &model->option);
  }

  for (XMLElement* section = FirstChildElement(root, "size"); section;
       section = NextSiblingElement(section, "size")) {
    Size(section, model);
  }

  //------------------ parse MJCF-specific sections

  for (XMLElement* section = FirstChildElement(root, "visual"); section;
       section = NextSiblingElement(section, "visual")) {
    Visual(section);
  }

  for (XMLElement* section = FirstChildElement(root, "statistic"); section;
       section = NextSiblingElement(section, "statistic")) {
    Statistic(section);
  }

  readingdefaults = true;
  for (XMLElement* section = FirstChildElement(root, "default"); section;
       section = NextSiblingElement(section, "default")) {
    Default(section, -1);
  }
  readingdefaults = false;

  for (XMLElement* section = FirstChildElement(root, "extension"); section;
       section = NextSiblingElement(section, "extension")) {
    Extension(section);
  }

  for (XMLElement* section = FirstChildElement(root, "custom"); section;
       section = NextSiblingElement(section, "custom")) {
    Custom(section);
  }

  for (XMLElement* section = FirstChildElement(root, "asset"); section;
       section = NextSiblingElement(section, "asset")) {
    Asset(section);
  }

  for (XMLElement* section = FirstChildElement(root, "worldbody"); section;
       section = NextSiblingElement(section, "worldbody")) {
    Body(section, &model->GetWorld()->spec, nullptr);
  }

  for (XMLElement* section = FirstChildElement(root, "contact"); section;
       section = NextSiblingElement(section, "contact")) {
    Contact(section);
  }

  for (XMLElement* section = FirstChildElement(root, "deformable"); section;
       section = NextSiblingElement(section, "deformable")) {
    Deformable(section);
  }

  for (XMLElement* section = FirstChildElement(root, "equality"); section;
       section = NextSiblingElement(section, "equality")) {
    Equality(section);
  }

  for (XMLElement* section = FirstChildElement(root, "tendon"); section;
       section = NextSiblingElement(section, "tendon")) {
    Tendon(section);
  }

  for (XMLElement* section = FirstChildElement(root, "actuator"); section;
       section = NextSiblingElement(section, "actuator")) {
    Actuator(section);
  }

  for (XMLElement* section = FirstChildElement(root, "sensor"); section;
       section = NextSiblingElement(section, "sensor")) {
    Sensor(section);
  }

  for (XMLElement* section = FirstChildElement(root, "keyframe"); section;
       section = NextSiblingElement(section, "keyframe")) {
    Keyframe(section);
  }
}



// compiler section parser
void mjXReader::Compiler(XMLElement* section, mjCModel* mod) {
  string text;
  int n;

  // top-level attributes
  if (MapValue(section, "autolimits", &n, bool_map, 2)) {
    mod->autolimits = (n==1);
  }
  ReadAttr(section, "boundmass", 1, &mod->boundmass, text);
  ReadAttr(section, "boundinertia", 1, &mod->boundinertia, text);
  ReadAttr(section, "settotalmass", 1, &mod->settotalmass, text);
  if (MapValue(section, "balanceinertia", &n, bool_map, 2)) {
    mod->balanceinertia = (n==1);
  }
  if (MapValue(section, "strippath", &n, bool_map, 2)) {
    mod->strippath = (n==1);
  }
  if (MapValue(section, "fitaabb", &n, bool_map, 2)) {
    mod->fitaabb = (n==1);
  }
  if (MapValue(section, "coordinate", &n, coordinate_map, 2)) {
    if (n==1) {
      throw mjXError(section, "global coordinates no longer supported. To convert existing models, "
                              "load and save them in MuJoCo 2.3.3 or older");
    }
  }
  if (MapValue(section, "angle", &n, angle_map, 2)) {
    mod->degree = (n==1);
  }
  if (ReadAttrTxt(section, "eulerseq", text)) {
    if (text.size()!=3) {
      throw mjXError(section, "euler format must have length 3");
    }
    memcpy(mod->euler, text.c_str(), 3);
  }
  if (ReadAttrTxt(section, "assetdir", text)) {
    mod->meshdir = text;
    mod->texturedir = text;
  }
  // meshdir and texturedir take precedence over assetdir
  ReadAttrTxt(section, "meshdir", mod->meshdir);
  ReadAttrTxt(section, "texturedir", mod->texturedir);
  if (MapValue(section, "discardvisual", &n, bool_map, 2)) {
    mod->discardvisual = (n==1);
  }
  if (MapValue(section, "convexhull", &n, bool_map, 2)) {
    mod->convexhull = (n==1);
  }
  if (MapValue(section, "usethread", &n, bool_map, 2)) {
    mod->usethread = (n==1);
  }
  if (MapValue(section, "fusestatic", &n, bool_map, 2)) {
    mod->fusestatic = (n==1);
  }
  MapValue(section, "inertiafromgeom", &mod->inertiafromgeom, TFAuto_map, 3);
  ReadAttr(section, "inertiagrouprange", 2, mod->inertiagrouprange, text);
  if (MapValue(section, "exactmeshinertia", &n, bool_map, 2)){
    mod->exactmeshinertia = (n==1);
  }

  // lengthrange subelement
  XMLElement* elem = FindSubElem(section, "lengthrange");
  if (elem) {
    mjLROpt* opt = &(mod->LRopt);

    // flags
    MapValue(elem, "mode", &opt->mode, lrmode_map, lrmode_sz);
    if (MapValue(elem, "useexisting", &n, bool_map, 2)) {
      opt->useexisting = (n==1);
    }
    if (MapValue(elem, "uselimit", &n, bool_map, 2)) {
      opt->uselimit = (n==1);
    }

    // algorithm parameters
    ReadAttr(elem, "accel", 1, &opt->accel, text);
    ReadAttr(elem, "maxforce", 1, &opt->maxforce, text);
    ReadAttr(elem, "timeconst", 1, &opt->timeconst, text);
    ReadAttr(elem, "timestep", 1, &opt->timestep, text);
    ReadAttr(elem, "inttotal", 1, &opt->inttotal, text);
    ReadAttr(elem, "interval", 1, &opt->interval, text);
    ReadAttr(elem, "tolrange", 1, &opt->tolrange, text);
  }
}



// option section parser
void mjXReader::Option(XMLElement* section, mjOption* opt) {
  string text;
  int n;

  // read options
  ReadAttr(section, "timestep", 1, &opt->timestep, text);
  ReadAttr(section, "apirate", 1, &opt->apirate, text);
  ReadAttr(section, "impratio", 1, &opt->impratio, text);
  ReadAttr(section, "tolerance", 1, &opt->tolerance, text);
  ReadAttr(section, "ls_tolerance", 1, &opt->ls_tolerance, text);
  ReadAttr(section, "noslip_tolerance", 1, &opt->noslip_tolerance, text);
  ReadAttr(section, "mpr_tolerance", 1, &opt->mpr_tolerance, text);
  ReadAttr(section, "gravity", 3, opt->gravity, text);
  ReadAttr(section, "wind", 3, opt->wind, text);
  ReadAttr(section, "magnetic", 3, opt->magnetic, text);
  ReadAttr(section, "density", 1, &opt->density, text);
  ReadAttr(section, "viscosity", 1, &opt->viscosity, text);

  ReadAttr(section, "o_margin", 1, &opt->o_margin, text);
  ReadAttr(section, "o_solref", mjNREF, opt->o_solref, text, false, false);
  ReadAttr(section, "o_solimp", mjNIMP, opt->o_solimp, text, false, false);
  ReadAttr(section, "o_friction", 5, opt->o_friction, text, false, false);

  MapValue(section, "integrator", &opt->integrator, integrator_map, integrator_sz);
  MapValue(section, "cone", &opt->cone, cone_map, cone_sz);
  MapValue(section, "jacobian", &opt->jacobian, jac_map, jac_sz);
  MapValue(section, "solver", &opt->solver, solver_map, solver_sz);
  ReadAttrInt(section, "iterations", &opt->iterations);
  ReadAttrInt(section, "ls_iterations", &opt->ls_iterations);
  ReadAttrInt(section, "noslip_iterations", &opt->noslip_iterations);
  ReadAttrInt(section, "mpr_iterations", &opt->mpr_iterations);
  ReadAttrInt(section, "sdf_iterations", &opt->sdf_iterations);
  ReadAttrInt(section, "sdf_initpoints", &opt->sdf_initpoints);

  // actuatorgroupdisable
  constexpr int num_bitflags = 31;
  int disabled_act_groups[num_bitflags];
  int num_found = ReadAttr(section, "actuatorgroupdisable", num_bitflags, disabled_act_groups,
                           text, false, false);
  for (int i=0; i < num_found; i++) {
    int group = disabled_act_groups[i];
    if (group < 0) {
      throw mjXError(section, "disabled actuator group value must be non-negative");
    }
    if (group > num_bitflags - 1) {
      throw mjXError(section, "disabled actuator group value cannot exceed 30");
    }
    opt->disableactuator |= (1 << group);
  }

  // read disable sub-element
  XMLElement* elem = FindSubElem(section, "flag");
  if (elem) {
#define READDSBL(NAME, MASK) \
        if (MapValue(elem, NAME, &n, enable_map, 2)) { \
            opt->disableflags ^= (opt->disableflags & MASK); \
            opt->disableflags |= (n ? 0 : MASK); }

    READDSBL("constraint",   mjDSBL_CONSTRAINT)
    READDSBL("equality",     mjDSBL_EQUALITY)
    READDSBL("frictionloss", mjDSBL_FRICTIONLOSS)
    READDSBL("limit",        mjDSBL_LIMIT)
    READDSBL("contact",      mjDSBL_CONTACT)
    READDSBL("passive",      mjDSBL_PASSIVE)
    READDSBL("gravity",      mjDSBL_GRAVITY)
    READDSBL("clampctrl",    mjDSBL_CLAMPCTRL)
    READDSBL("warmstart",    mjDSBL_WARMSTART)
    READDSBL("filterparent", mjDSBL_FILTERPARENT)
    READDSBL("actuation",    mjDSBL_ACTUATION)
    READDSBL("refsafe",      mjDSBL_REFSAFE)
    READDSBL("sensor",       mjDSBL_SENSOR)
    READDSBL("midphase",     mjDSBL_MIDPHASE)
    READDSBL("eulerdamp",    mjDSBL_EULERDAMP)
#undef READDSBL

#define READENBL(NAME, MASK) \
        if (MapValue(elem, NAME, &n, enable_map, 2)) { \
            opt->enableflags ^= (opt->enableflags & MASK); \
            opt->enableflags |= (n ? MASK : 0); }

    READENBL("override",    mjENBL_OVERRIDE)
    READENBL("energy",      mjENBL_ENERGY)
    READENBL("fwdinv",      mjENBL_FWDINV)
    READENBL("invdiscrete", mjENBL_INVDISCRETE)
    READENBL("sensornoise", mjENBL_SENSORNOISE)
    READENBL("multiccd",    mjENBL_MULTICCD)
    READENBL("island",      mjENBL_ISLAND)
#undef READENBL
  }
}



// size section parser
void mjXReader::Size(XMLElement* section, mjCModel* mod) {
  // read memory bytes
  {
    constexpr char err_msg[] =
        "unsigned integer with an optional suffix {K,M,G,T,P,E} is expected in "
        "attribute 'memory' (or the size specified is too big)";

    auto memory = [&]() -> std::optional<std::size_t> {
      const char* pstr = section->Attribute("memory");
      if (!pstr) {
        return std::nullopt;
      }

      // trim entire string
      std::string trimmed;
      {
        std::istringstream strm((std::string(pstr)));
        strm >> trimmed;
        std::string trailing;
        strm >> trailing;
        if (!trailing.empty() || !strm.eof()) {
          throw mjXError(section, "%s", err_msg);
        }

        // allow explicit specification of the default "-1" value
        if (trimmed == "-1") {
          return std::nullopt;
        }
      }

      std::istringstream strm(trimmed);

      // check that the number is not negative
      if (strm.peek() == '-') {
        throw mjXError(section, "%s", err_msg);
      }

      std::size_t base_size;
      strm >> base_size;
      if (strm.fail()) {
        // either not an integer or the number without the suffix is already bigger than size_t
        throw mjXError(section, "%s", err_msg);
      }

      // parse the multiplier suffix
      int multiplier_bit = 0;
      if (!strm.eof()) {
        char suffix = strm.get();
        if (suffix == 'K' || suffix == 'k') {
          multiplier_bit = 10;
        } else if (suffix == 'M' || suffix == 'm') {
          multiplier_bit = 20;
        } else if (suffix == 'G' || suffix == 'g') {
          multiplier_bit = 30;
        } else if (suffix == 'T' || suffix == 't') {
          multiplier_bit = 40;
        } else if (suffix == 'P' || suffix == 'p') {
          multiplier_bit = 50;
        } else if (suffix == 'E' || suffix == 'e') {
          multiplier_bit = 60;
        }

        // check for invalid suffix, or suffix longer than one character
        strm.get();
        if (!multiplier_bit || !strm.eof()) {
          throw mjXError(section, "%s", err_msg);
        }
      }

      // check that the specified suffix isn't bigger than size_t
      if (multiplier_bit + 1 > std::numeric_limits<std::size_t>::digits) {
        throw mjXError(section, "%s", err_msg);
      }

      // check that the suffix won't take the total size beyond size_t
      const std::size_t max_base_size =
          (std::numeric_limits<std::size_t>::max() << multiplier_bit) >> multiplier_bit;
      if (base_size > max_base_size) {
        throw mjXError(section, "%s", err_msg);
      }

      const std::size_t total_size = base_size << multiplier_bit;
      return total_size;
    }();

    if (memory.has_value()) {
      if (*memory / sizeof(mjtNum) > std::numeric_limits<int>::max()) {
        throw mjXError(section, "%s", err_msg);
      }
      mod->memory = *memory;
    }
  }

  // read sizes
  ReadAttrInt(section, "nuserdata", &mod->nuserdata);
  ReadAttrInt(section, "nkey", &mod->nkey);

  ReadAttrInt(section, "nconmax", &mod->nconmax);
  if (mod->nconmax < -1) throw mjXError(section, "nconmax must be >= -1");

  {
    int nstack = -1;
    const bool has_nstack = ReadAttrInt(section, "nstack", &nstack);
    if (has_nstack) {
      if (mod->nstack < -1) {
        throw mjXError(section, "nstack must be >= -1");
      }
      if (mod->memory != -1 && nstack != -1) {
        throw mjXError(section,
                       "either 'memory' and 'nstack' attribute can be specified, not both");
      }
      mod->nstack = nstack;
    }
  }
  {
    int njmax = -1;
    const bool has_njmax = ReadAttrInt(section, "njmax", &njmax);
    if (has_njmax) {
      if (mod->njmax < -1) {
        throw mjXError(section, "njmax must be >= -1");
      }
      if (mod->memory != -1 && njmax != -1) {
        throw mjXError(section,
                       "either 'memory' and 'njmax' attribute can be specified, not both");
      }
      mod->njmax = njmax;
    }
  }

  ReadAttrInt(section, "nuser_body", &mod->nuser_body);
  if (mod->nuser_body < -1) throw mjXError(section, "nuser_body must be >= -1");

  ReadAttrInt(section, "nuser_jnt", &mod->nuser_jnt);
  if (mod->nuser_jnt < -1) throw mjXError(section, "nuser_jnt must be >= -1");

  ReadAttrInt(section, "nuser_geom", &mod->nuser_geom);
  if (mod->nuser_geom < -1) throw mjXError(section, "nuser_geom must be >= -1");

  ReadAttrInt(section, "nuser_site", &mod->nuser_site);
  if (mod->nuser_site < -1) throw mjXError(section, "nuser_site must be >= -1");

  ReadAttrInt(section, "nuser_cam", &mod->nuser_cam);
  if (mod->nuser_cam < -1) throw mjXError(section, "nuser_cam must be >= -1");

  ReadAttrInt(section, "nuser_tendon", &mod->nuser_tendon);
  if (mod->nuser_tendon < -1) throw mjXError(section, "nuser_tendon must be >= -1");

  ReadAttrInt(section, "nuser_actuator", &mod->nuser_actuator);
  if (mod->nuser_actuator < -1) throw mjXError(section, "nuser_actuator must be >= -1");

  ReadAttrInt(section, "nuser_sensor", &mod->nuser_sensor);
  if (mod->nuser_sensor < -1) throw mjXError(section, "nuser_sensor must be >= -1");
}



// statistic section parser
void mjXReader::Statistic(XMLElement* section) {
  string text;

  // read statistics
  ReadAttr(section, "meaninertia", 1, &model->meaninertia, text);
  ReadAttr(section, "meanmass", 1, &model->meanmass, text);
  ReadAttr(section, "meansize", 1, &model->meansize, text);
  ReadAttr(section, "extent", 1, &model->extent, text);
  if (mjuu_defined(model->extent) && model->extent<=0) {
    throw mjXError(section, "extent must be strictly positive");
  }
  ReadAttr(section, "center", 3, model->center, text);
}



//---------------------------------- one-element parsers -------------------------------------------

// flex element parser
void mjXReader::OneFlex(XMLElement* elem, mjmFlex* pflex) {
  string text, name, classname, material;
  int n;

  // read attributes
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(pflex->name, name.c_str());
  }
  if (ReadAttrTxt(elem, "classname", classname)) {
    mjm_setString(pflex->classname, classname.c_str());
  }
  if (ReadAttrTxt(elem, "material", material)) {
    mjm_setString(pflex->material, material.c_str());
  }

  ReadAttr(elem, "radius", 1, &pflex->radius, text);
  ReadAttr(elem, "rgba", 4, pflex->rgba, text);
  if (MapValue(elem, "flatskin", &n, bool_map, 2)) {
    pflex->flatskin = (n==1);
  }
  ReadAttrInt(elem, "dim", &pflex->dim);
  ReadAttrInt(elem, "group", &pflex->group);

  // read data vectors
  if (ReadAttrTxt(elem, "body", text, true)) {
    mjm_setStringVec(pflex->vertbody, text.c_str());
  }
  if (ReadAttrTxt(elem, "vertex", text)) {
    std::vector<double> vert;
    String2Vector(text, vert);
    mjm_setDouble(pflex->vert, vert.data(), vert.size());
  }
  if (ReadAttrTxt(elem, "element", text, true)) {
    std::vector<int> elem;
    String2Vector(text, elem);
    mjm_setInt(pflex->elem, elem.data(), elem.size());
  }
  if (ReadAttrTxt(elem, "texcoord", text)) {
    std::vector<float> texcoord;
    String2Vector(text, texcoord);
    mjm_setFloat(pflex->texcoord, texcoord.data(), texcoord.size());
  }

  // contact subelement
  XMLElement* cont = FirstChildElement(elem, "contact");
  if (cont) {
    ReadAttrInt(cont, "contype", &pflex->contype);
    ReadAttrInt(cont, "conaffinity", &pflex->conaffinity);
    ReadAttrInt(cont, "condim", &pflex->condim);
    ReadAttrInt(cont, "priority", &pflex->priority);
    ReadAttr(cont, "friction", 3, pflex->friction, text, false, false);
    ReadAttr(cont, "solmix", 1, &pflex->solmix, text);
    ReadAttr(cont, "solref", mjNREF, pflex->solref, text, false, false);
    ReadAttr(cont, "solimp", mjNIMP, pflex->solimp, text, false, false);
    ReadAttr(cont, "margin", 1, &pflex->margin, text);
    ReadAttr(cont, "gap", 1, &pflex->gap, text);
    if (MapValue(cont, "internal", &n, bool_map, 2)) {
      pflex->internal = (n==1);
    }
    MapValue(cont, "selfcollide", &pflex->selfcollide, flexself_map, 5);
    ReadAttrInt(cont, "activelayers", &pflex->activelayers);
  }

  // edge subelement
  XMLElement* edge = FirstChildElement(elem, "edge");
  if (edge) {
    ReadAttr(edge, "stiffness", 1, &pflex->edgestiffness, text);
    ReadAttr(edge, "damping", 1, &pflex->edgedamping, text);
  }

  // write error info
  mjm_setString(pflex->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// mesh element parser
void mjXReader::OneMesh(XMLElement* elem, mjmMesh* pmesh) {
  int n;
  string text, name, classname, content_type, file;

  // read attributes
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(pmesh->name, name.c_str());
  }
  if (ReadAttrTxt(elem, "class", classname)) {
    mjm_setString(pmesh->classname, classname.c_str());
  }
  if (ReadAttrTxt(elem, "content_type", content_type)) {
    mjm_setString(pmesh->content_type, content_type.c_str());
  }
  if (ReadAttrTxt(elem, "file", file)) {
    mjm_setString(pmesh->file, file.c_str());
  }
  ReadAttr(elem, "refpos", 3, pmesh->refpos, text);
  ReadAttr(elem, "refpos", 4, pmesh->refquat, text);
  ReadAttr(elem, "scale", 3, pmesh->scale, text);

  XMLElement* eplugin = FirstChildElement(elem, "plugin");
  if (eplugin) {
    OnePlugin(eplugin, &pmesh->plugin);
  }

  if (MapValue(elem, "smoothnormal", &n, bool_map, 2)) {
    pmesh->smoothnormal = (n==1);
  }

  // read user vertex data
  if (ReadAttrTxt(elem, "vertex", text)) {
    auto uservert = ReadAttrVec<float>(elem, "vertex");
    if (uservert.has_value()) {
      mjm_setFloat(pmesh->uservert, uservert->data(), uservert->size());
    }
  }

  // read user normal data
  if (ReadAttrTxt(elem, "normal", text)) {
    auto usernormal = ReadAttrVec<float>(elem, "normal");
    if (usernormal.has_value()) {
      mjm_setFloat(pmesh->usernormal, usernormal->data(), usernormal->size());
    }
  }

  // read user texcoord data
  if (ReadAttrTxt(elem, "texcoord", text)) {
    auto usertexcoord = ReadAttrVec<float>(elem, "texcoord");
    if (usertexcoord.has_value()) {
      mjm_setFloat(pmesh->usertexcoord, usertexcoord->data(), usertexcoord->size());
    }
  }

  // read user face data
  if (ReadAttrTxt(elem, "face", text)) {
    auto userface = ReadAttrVec<int>(elem, "face");
    if (userface.has_value()) {
      mjm_setInt(pmesh->userface, userface->data(), userface->size());
    }
  }

  // write error info
  mjm_setString(pmesh->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// skin element parser
void mjXReader::OneSkin(XMLElement* elem, mjCSkin* pskin) {
  string text;
  float data[4];

  // read attributes
  ReadAttrTxt(elem, "name", pskin->name);
  ReadAttrTxt(elem, "file", pskin->file);
  ReadAttrTxt(elem, "material", pskin->get_material());
  ReadAttrInt(elem, "group", &pskin->group);
  if (pskin->group<0 || pskin->group>=mjNGROUP) {
    throw mjXError(elem, "skin group must be between 0 and 5");
  }
  ReadAttr(elem, "rgba", 4, pskin->rgba, text);
  ReadAttr(elem, "inflate", 1, &pskin->inflate, text);

  // read vertex data
  if (ReadAttrTxt(elem, "vertex", text)) String2Vector(text, pskin->vert);

  // read texcoord data
  if (ReadAttrTxt(elem, "texcoord", text)) String2Vector(text, pskin->texcoord);

  // read user face data
  if (ReadAttrTxt(elem, "face", text)) String2Vector(text, pskin->face);

  // read bones
  XMLElement* bone = FirstChildElement(elem, "bone");
  while (bone) {
    // read body
    ReadAttrTxt(bone, "body", text, true);
    pskin->bodyname.push_back(text);

    // read bindpos
    ReadAttr(bone, "bindpos", 3, data, text, true);
    pskin->bindpos.push_back(data[0]);
    pskin->bindpos.push_back(data[1]);
    pskin->bindpos.push_back(data[2]);

    // read bindquat
    ReadAttr(bone, "bindquat", 4, data, text, true);
    pskin->bindquat.push_back(data[0]);
    pskin->bindquat.push_back(data[1]);
    pskin->bindquat.push_back(data[2]);
    pskin->bindquat.push_back(data[3]);

    // read vertid
    vector<int> tempid;
    ReadAttrTxt(bone, "vertid", text, true);
    String2Vector(text, tempid);
    pskin->vertid.push_back(tempid);

    // read vertweight
    vector<float> tempweight;
    ReadAttrTxt(bone, "vertweight", text, true);
    String2Vector(text, tempweight);
    pskin->vertweight.push_back(tempweight);

    // advance to next bone
    bone = NextSiblingElement(bone, "bone");
  }

  GetXMLPos(elem, pskin);
}



// material element parser
void mjXReader::OneMaterial(XMLElement* elem, mjmMaterial* pmat) {
  string text, name, classname, texture;
  int n;

  // read attributes
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(pmat->name, name.c_str());
  }
  if (ReadAttrTxt(elem, "class", classname)) {
    mjm_setString(pmat->classname, classname.c_str());
  }
  if (ReadAttrTxt(elem, "texture", texture)) {
    mjm_setString(pmat->texture, texture.c_str());
  }
  if (MapValue(elem, "texuniform", &n, bool_map, 2)) {
    pmat->texuniform = (n==1);
  }
  ReadAttr(elem, "texrepeat", 2, pmat->texrepeat, text);
  ReadAttr(elem, "emission", 1, &pmat->emission, text);
  ReadAttr(elem, "specular", 1, &pmat->specular, text);
  ReadAttr(elem, "shininess", 1, &pmat->shininess, text);
  ReadAttr(elem, "reflectance", 1, &pmat->reflectance, text);
  ReadAttr(elem, "rgba", 4, pmat->rgba, text);

  // write error info
  mjm_setString(pmat->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// joint element parser
void mjXReader::OneJoint(XMLElement* elem, mjmJoint* pjoint) {
  string text, name, classname;
  std::vector<double> userdata;
  int n;

  // read attributes
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(pjoint->name, name.c_str());
  }
  if (ReadAttrTxt(elem, "class", classname)) {
    mjm_setString(pjoint->classname, classname.c_str());
  }
  if (MapValue(elem, "type", &n, joint_map, joint_sz)) {
    pjoint->type = (mjtJoint)n;
  }
  MapValue(elem, "limited", &pjoint->limited, TFAuto_map, 3);
  MapValue(elem, "actuatorfrclimited", &pjoint->actfrclimited, TFAuto_map, 3);
  ReadAttrInt(elem, "group", &pjoint->group);
  ReadAttr(elem, "solreflimit", mjNREF, pjoint->solref_limit, text, false, false);
  ReadAttr(elem, "solimplimit", mjNIMP, pjoint->solimp_limit, text, false, false);
  ReadAttr(elem, "solreffriction", mjNREF, pjoint->solref_friction, text, false, false);
  ReadAttr(elem, "solimpfriction", mjNIMP, pjoint->solimp_friction, text, false, false);
  ReadAttr(elem, "pos", 3, pjoint->pos, text);
  ReadAttr(elem, "axis", 3, pjoint->axis, text);
  ReadAttr(elem, "springdamper", 2, pjoint->springdamper, text);
  ReadAttr(elem, "stiffness", 1, &pjoint->stiffness, text);
  ReadAttr(elem, "range", 2, pjoint->range, text);
  ReadAttr(elem, "actuatorfrcrange", 2, pjoint->actfrcrange, text);
  ReadAttr(elem, "margin", 1, &pjoint->margin, text);
  ReadAttr(elem, "ref", 1, &pjoint->ref, text);
  ReadAttr(elem, "springref", 1, &pjoint->springref, text);
  ReadAttr(elem, "armature", 1, &pjoint->armature, text);
  ReadAttr(elem, "damping", 1, &pjoint->damping, text);
  ReadAttr(elem, "frictionloss", 1, &pjoint->frictionloss, text);

  // read userdata
  if (ReadVector(elem, "user", userdata, text)) {
    mjm_setDouble(pjoint->userdata, userdata.data(), userdata.size());
  }

  // write error info
  mjm_setString(pjoint->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// geom element parser
void mjXReader::OneGeom(XMLElement* elem, mjmGeom* pgeom) {
  string text, name, classname;
  std::vector<double> userdata;
  std::string hfieldname, meshname, material;
  int n;

  // read attributes
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(pgeom->name, name.c_str());
  }
  if (ReadAttrTxt(elem, "class", classname)) {
    mjm_setString(pgeom->classname, classname.c_str());
  }
  if (MapValue(elem, "type", &n, geom_map, mjNGEOMTYPES)) {
    pgeom->type = (mjtGeom)n;
  }
  ReadAttr(elem, "size", 3, pgeom->size, text, false, false);
  ReadAttrInt(elem, "contype", &pgeom->contype);
  ReadAttrInt(elem, "conaffinity", &pgeom->conaffinity);
  ReadAttrInt(elem, "condim", &pgeom->condim);
  ReadAttrInt(elem, "group", &pgeom->group);
  ReadAttrInt(elem, "priority", &pgeom->priority);
  ReadAttr(elem, "friction", 3, pgeom->friction, text, false, false);
  ReadAttr(elem, "solmix", 1, &pgeom->solmix, text);
  ReadAttr(elem, "solref", mjNREF, pgeom->solref, text, false, false);
  ReadAttr(elem, "solimp", mjNIMP, pgeom->solimp, text, false, false);
  ReadAttr(elem, "margin", 1, &pgeom->margin, text);
  ReadAttr(elem, "gap", 1, &pgeom->gap, text);
  if (ReadAttrTxt(elem, "hfield", hfieldname)) {
    mjm_setString(pgeom->hfieldname, hfieldname.c_str());
  }
  if (ReadAttrTxt(elem, "mesh", meshname)) {
    mjm_setString(pgeom->meshname, meshname.c_str());
  }
  ReadAttr(elem, "fitscale", 1, &pgeom->fitscale, text);
  if (ReadAttrTxt(elem, "material", material)) {
    mjm_setString(pgeom->material, material.c_str());
  }
  ReadAttr(elem, "rgba", 4, pgeom->rgba, text);
  if (MapValue(elem, "fluidshape", &n, fluid_map, 2)) {
    pgeom->fluid_ellipsoid = (n == 1);
  }
  ReadAttr(elem, "fluidcoef", 5, pgeom->fluid_coefs, text, false, false);

  // read userdata
  if (ReadVector(elem, "user", userdata, text)) {
    mjm_setDouble(pgeom->userdata, userdata.data(), userdata.size());
  }

  // plugin sub-element
  XMLElement* eplugin = FirstChildElement(elem, "plugin");
  if (eplugin) {
    OnePlugin(eplugin, &pgeom->plugin);
  }

  // remaining attributes
  ReadAttr(elem, "mass", 1, &pgeom->mass, text);
  ReadAttr(elem, "density", 1, &pgeom->density, text);
  ReadAttr(elem, "fromto", 6, pgeom->fromto, text);
  ReadAttr(elem, "pos", 3, pgeom->pos, text);
  ReadQuat(elem, "quat", pgeom->quat, text);
  ReadAlternative(elem, pgeom->alt);

  // compute inertia using either solid or shell geometry
  if (MapValue(elem, "shellinertia", &n, meshtype_map, 2)) {
    pgeom->typeinertia = (mjtGeomInertia)n;
  }

  // write error info
  mjm_setString(pgeom->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// site element parser
void mjXReader::OneSite(XMLElement* elem, mjmSite& site) {
  int n;
  string text, name, classname;
  std::vector<double> userdata;
  std::string material;

  // read attributes
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(site.name, name.c_str());
  }
  if (ReadAttrTxt(elem, "class", classname)) {
    mjm_setString(site.classname, classname.c_str());
  }
  if (MapValue(elem, "type", &n, geom_map, mjNGEOMTYPES)) {
    site.type = (mjtGeom)n;
  }
  ReadAttr(elem, "size", 3, site.size, text, false, false);
  ReadAttrInt(elem, "group", &site.group);
  ReadAttr(elem, "pos", 3, site.pos, text);
  ReadQuat(elem, "quat", site.quat, text);
  if (ReadAttrTxt(elem, "material", material)) {
    mjm_setString(site.material, material.c_str());
  }
  ReadAttr(elem, "rgba", 4, site.rgba, text);
  ReadAttr(elem, "fromto", 6, site.fromto, text);
  ReadAlternative(elem, site.alt);
  if (ReadVector(elem, "user", userdata, text)) {
    mjm_setDouble(site.userdata, userdata.data(), userdata.size());
  }

  // write error info
  mjm_setString(site.info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// camera element parser
void mjXReader::OneCamera(XMLElement* elem, mjmCamera* pcam) {
  int n;
  string text, name, classname, targetbody;
  std::vector<double> userdata;

  // read attributes
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(pcam->name, name.c_str());
  }
  if (ReadAttrTxt(elem, "class", classname)) {
    mjm_setString(pcam->classname, classname.c_str());
  }
  if (ReadAttrTxt(elem, "target", targetbody)) {
    mjm_setString(pcam->targetbody, targetbody.c_str());
  }
  if (MapValue(elem, "mode", &n, camlight_map, camlight_sz)) {
    pcam->mode = (mjtCamLight)n;
  }
  ReadAttr(elem, "pos", 3, pcam->pos, text);
  ReadQuat(elem, "quat", pcam->quat, text);
  ReadAlternative(elem, pcam->alt);
  ReadAttr(elem, "ipd", 1, &pcam->ipd, text);

  bool has_principal = ReadAttr(elem, "principalpixel", 2, pcam->principal_pixel, text) ||
                       ReadAttr(elem, "principal", 2, pcam->principal_length, text);
  bool has_focal = ReadAttr(elem, "focalpixel", 2, pcam->focal_pixel, text) ||
                   ReadAttr(elem, "focal", 2, pcam->focal_length, text);
  bool needs_sensorsize = has_principal || has_focal;
  bool has_sensorsize = ReadAttr(elem, "sensorsize", 2, pcam->sensor_size, text, needs_sensorsize);
  bool has_fovy = ReadAttr(elem, "fovy", 1, &pcam->fovy, text);
  bool needs_resolution = has_focal || has_sensorsize;
  ReadAttr(elem, "resolution", 2, pcam->resolution, text, needs_resolution);

  if (pcam->resolution[0] < 0 || pcam->resolution[1] < 0) {
    throw mjXError(elem, "camera resolution cannot be negative");
  }

  if (has_fovy && has_sensorsize) {
    throw mjXError(
        elem,
        "either 'fovy' or 'sensorsize' attribute can be specified, not both");
  }

  // read userdata
  ReadVector(elem, "user", userdata, text);
  mjm_setDouble(pcam->userdata, userdata.data(), userdata.size());

  // write error info
  mjm_setString(pcam->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// light element parser
void mjXReader::OneLight(XMLElement* elem, mjmLight* plight) {
  int n;
  string text, name, classname, targetbody;

  // read attributes
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(plight->name, name.c_str());
  }
  if (ReadAttrTxt(elem, "class", classname)) {
    mjm_setString(plight->classname, classname.c_str());
  }
  if (ReadAttrTxt(elem, "target", targetbody)) {
    mjm_setString(plight->targetbody, targetbody.c_str());
  }
  if (MapValue(elem, "mode", &n, camlight_map, camlight_sz)) {
    plight->mode = (mjtCamLight)n;
  }
  if (MapValue(elem, "directional", &n, bool_map, 2)) {
    plight->directional = (n==1);
  }
  if (MapValue(elem, "castshadow", &n, bool_map, 2)) {
    plight->castshadow = (n==1);
  }
  if (MapValue(elem, "active", &n, bool_map, 2)) {
    plight->active = (n==1);
  }
  ReadAttr(elem, "pos", 3, plight->pos, text);
  ReadAttr(elem, "dir", 3, plight->dir, text);
  ReadAttr(elem, "attenuation", 3, plight->attenuation, text);
  ReadAttr(elem, "cutoff", 1, &plight->cutoff, text);
  ReadAttr(elem, "exponent", 1, &plight->exponent, text);
  ReadAttr(elem, "ambient", 3, plight->ambient, text);
  ReadAttr(elem, "diffuse", 3, plight->diffuse, text);
  ReadAttr(elem, "specular", 3, plight->specular, text);

  // write error info
  mjm_setString(plight->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// pair element parser
void mjXReader::OnePair(XMLElement* elem, mjmPair* ppair) {
  string text, name, classname, geomname1, geomname2;

  // regular only
  if (!readingdefaults) {
    if (ReadAttrTxt(elem, "class", classname)) {
      mjm_setString(ppair->classname, classname.c_str());
    }
    if (ReadAttrTxt(elem, "geom1", geomname1)) {
      mjm_setString(ppair->geomname1, geomname1.c_str());
    }
    if (ReadAttrTxt(elem, "geom2", geomname2)) {
      mjm_setString(ppair->geomname2, geomname2.c_str());
    }
  }

  // read other parameters
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(ppair->name, name.c_str());
  }
  ReadAttrInt(elem, "condim", &ppair->condim);
  ReadAttr(elem, "solref", mjNREF, ppair->solref, text, false, false);
  ReadAttr(elem, "solreffriction", mjNREF, ppair->solreffriction, text, false, false);
  ReadAttr(elem, "solimp", mjNIMP, ppair->solimp, text, false, false);
  ReadAttr(elem, "margin", 1, &ppair->margin, text);
  ReadAttr(elem, "gap", 1, &ppair->gap, text);
  ReadAttr(elem, "friction", 5, ppair->friction, text, false, false);

  // write error info
  mjm_setString(ppair->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// equality element parser
void mjXReader::OneEquality(XMLElement* elem, mjmEquality* pequality) {
  int n;
  string text, name1, name2, name, classname;

  // read type (bad keywords already detected by schema)
  text = elem->Value();
  pequality->type = (mjtEq)FindKey(equality_map, equality_sz, text);

  // regular only
  if (!readingdefaults) {
    if (ReadAttrTxt(elem, "name", name)) {
      mjm_setString(pequality->name, name.c_str());
    }
    if (ReadAttrTxt(elem, "class", classname)) {
      mjm_setString(pequality->classname, classname.c_str());
    };

    switch (pequality->type) {
    case mjEQ_CONNECT:
      ReadAttrTxt(elem, "body1", name1, true);
      ReadAttrTxt(elem, "body2", name2);
      ReadAttr(elem, "anchor", 3, pequality->data, text, true);
      break;

    case mjEQ_WELD:
      ReadAttrTxt(elem, "body1", name1, true);
      ReadAttrTxt(elem, "body2", name2);
      ReadAttr(elem, "relpose", 7, pequality->data+3, text);
      ReadAttr(elem, "torquescale", 1, pequality->data+10, text);
      if (!ReadAttr(elem, "anchor", 3, pequality->data, text)) {
        mjuu_zerovec(pequality->data, 3);
      }
      break;

    case mjEQ_JOINT:
      ReadAttrTxt(elem, "joint1", name1, true);
      ReadAttrTxt(elem, "joint2", name2);
      ReadAttr(elem, "polycoef", 5, pequality->data, text);
      break;

    case mjEQ_TENDON:
      ReadAttrTxt(elem, "tendon1", name1, true);
      ReadAttrTxt(elem, "tendon2", name2);
      ReadAttr(elem, "polycoef", 5, pequality->data, text);
      break;

    case mjEQ_FLEX:
      ReadAttrTxt(elem, "flex", name1, true);
      break;

    case mjEQ_DISTANCE:
      throw mjXError(elem, "support for distance equality constraints was removed in MuJoCo 2.2.2");
      break;

    default:                    // SHOULD NOT OCCUR
      throw mjXError(elem, "unrecognized equality constraint type");
    }

    mjm_setString(pequality->name1, name1.c_str());
    if (!name2.empty()) {
      mjm_setString(pequality->name2, name2.c_str());
    }
  }

  // read attributes
  if (MapValue(elem, "active", &n, bool_map, 2)) {
    pequality->active = (n==1);
  }
  ReadAttr(elem, "solref", mjNREF, pequality->solref, text, false, false);
  ReadAttr(elem, "solimp", mjNIMP, pequality->solimp, text, false, false);

  // write error info
  mjm_setString(pequality->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// tendon element parser
void mjXReader::OneTendon(XMLElement* elem, mjmTendon* pten) {
  string text, name, classname, material;
  std::vector<double> userdata;

  // read attributes
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(pten->name, name.c_str());
  }
  if (ReadAttrTxt(elem, "class", classname)) {
    mjm_setString(pten->classname, classname.c_str());
  }
  ReadAttrInt(elem, "group", &pten->group);
  if (ReadAttrTxt(elem, "material", material)) {
    mjm_setString(pten->material, material.c_str());
  }
  MapValue(elem, "limited", &pten->limited, TFAuto_map, 3);
  ReadAttr(elem, "width", 1, &pten->width, text);
  ReadAttr(elem, "solreflimit", mjNREF, pten->solref_limit, text, false, false);
  ReadAttr(elem, "solimplimit", mjNIMP, pten->solimp_limit, text, false, false);
  ReadAttr(elem, "solreffriction", mjNREF, pten->solref_friction, text, false, false);
  ReadAttr(elem, "solimpfriction", mjNIMP, pten->solimp_friction, text, false, false);
  ReadAttr(elem, "range", 2, pten->range, text);
  ReadAttr(elem, "margin", 1, &pten->margin, text);
  ReadAttr(elem, "stiffness", 1, &pten->stiffness, text);
  ReadAttr(elem, "damping", 1, &pten->damping, text);
  ReadAttr(elem, "frictionloss", 1, &pten->frictionloss, text);
  // read springlength, either one or two values; if one, copy to second value
  if (ReadAttr(elem, "springlength", 2, pten->springlength, text, false, false) == 1) {
    pten->springlength[1] = pten->springlength[0];
  }
  ReadAttr(elem, "rgba", 4, pten->rgba, text);

  // read userdata
  if (ReadVector(elem, "user", userdata, text)) {
    mjm_setDouble(pten->userdata, userdata.data(), userdata.size());
  }

  // write error info
  mjm_setString(pten->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// actuator element parser
void mjXReader::OneActuator(XMLElement* elem, mjmActuator* pact) {
  string text, type, name, classname, target, slidersite, refsite;

  // common attributes
  if (ReadAttrTxt(elem, "name", name)) {
    mjm_setString(pact->name, name.c_str());
  }
  if (ReadAttrTxt(elem, "class", classname)) {
    mjm_setString(pact->classname, classname.c_str());
  }
  ReadAttrInt(elem, "group", &pact->group);
  MapValue(elem, "ctrllimited", &pact->ctrllimited, TFAuto_map, 3);
  MapValue(elem, "forcelimited", &pact->forcelimited, TFAuto_map, 3);
  MapValue(elem, "actlimited", &pact->actlimited, TFAuto_map, 3);
  ReadAttr(elem, "ctrlrange", 2, pact->ctrlrange, text);
  ReadAttr(elem, "forcerange", 2, pact->forcerange, text);
  ReadAttr(elem, "actrange", 2, pact->actrange, text);
  ReadAttr(elem, "lengthrange", 2, pact->lengthrange, text);
  ReadAttr(elem, "gear", 6, pact->gear, text, false, false);

  // transmission target and type
  int cnt = 0;
  if (ReadAttrTxt(elem, "joint", target)) {
    mjm_setString(pact->target, target.c_str());
    pact->trntype = mjTRN_JOINT;
    cnt++;
  }
  if (ReadAttrTxt(elem, "jointinparent", target)) {
    mjm_setString(pact->target, target.c_str());
    pact->trntype = mjTRN_JOINTINPARENT;
    cnt++;
  }
  if (ReadAttrTxt(elem, "tendon", target)) {
    mjm_setString(pact->target, target.c_str());
    pact->trntype = mjTRN_TENDON;
    cnt++;
  }
  if (ReadAttrTxt(elem, "cranksite", target)) {
    mjm_setString(pact->target, target.c_str());
    pact->trntype = mjTRN_SLIDERCRANK;
    cnt++;
  }
  if (ReadAttrTxt(elem, "site", target)) {
    mjm_setString(pact->target, target.c_str());
    pact->trntype = mjTRN_SITE;
    cnt++;
  }
  if (ReadAttrTxt(elem, "body", target)) {
    mjm_setString(pact->target, target.c_str());
    pact->trntype = mjTRN_BODY;
    cnt++;
  }
  // check for repeated transmission
  if (cnt>1) {
    throw mjXError(elem, "actuator can have at most one of transmission target");
  }

  // slidercrank-specific parameters
  int r1 = ReadAttr(elem, "cranklength", 1, &pact->cranklength, text);
  int r2 = ReadAttrTxt(elem, "slidersite", slidersite);
  if (r2) {
    mjm_setString(pact->slidersite, slidersite.c_str());
  }
  if ((r1 || r2) && pact->trntype!=mjTRN_SLIDERCRANK && pact->trntype!=mjTRN_UNDEFINED) {
    throw mjXError(elem, "cranklength and slidersite can only be used in slidercrank transmission");
  }

  // site-specific parameters (refsite)
  int r3 = ReadAttrTxt(elem, "refsite", refsite);
  if (r3) {
    mjm_setString(pact->refsite, refsite.c_str());
  }
  if (r3 && pact->trntype!=mjTRN_SITE && pact->trntype!=mjTRN_UNDEFINED) {
    throw mjXError(elem, "refsite can only be used with site transmission");
  }

  // get predefined type
  type = elem->Value();

  // explicit attributes
  if (type=="general") {
    // explicit attributes
    int n;
    if (MapValue(elem, "dyntype", &n, dyn_map, dyn_sz)) {
      pact->dyntype = (mjtDyn)n;
    }
    if (MapValue(elem, "gaintype", &n, gain_map, gain_sz)) {
      pact->gaintype = (mjtGain)n;
    }
    if (MapValue(elem, "biastype", &n, bias_map, bias_sz)) {
      pact->biastype = (mjtBias)n;
    }
    if (MapValue(elem, "actearly", &n, bool_map, 2)) {
      pact->actearly = (n==1);
    }
    ReadAttr(elem, "dynprm", mjNDYN, pact->dynprm, text, false, false);
    ReadAttr(elem, "gainprm", mjNGAIN, pact->gainprm, text, false, false);
    ReadAttr(elem, "biasprm", mjNBIAS, pact->biasprm, text, false, false);
    ReadAttrInt(elem, "actdim", &pact->actdim);
  }

  // direct drive motor
  else if (type=="motor") {
    // unit gain
    pact->gainprm[0] = 1;

    // implied parameters
    pact->dyntype = mjDYN_NONE;
    pact->gaintype = mjGAIN_FIXED;
    pact->biastype = mjBIAS_NONE;
  }

  // position or integrated velocity servo
  else if (type=="position" || type=="intvelocity") {
    // explicit attributes
    ReadAttr(elem, "kp", 1, pact->gainprm, text);
    pact->biasprm[1] = -pact->gainprm[0];

    if (ReadAttr(elem, "kv", 1, pact->biasprm + 2, text)) {
      if (pact->biasprm[2] < 0)
        throw mjXError(elem, "kv cannot be negative");
      pact->biasprm[2] *= -1;
    }

    // implied parameters
    pact->gaintype = mjGAIN_FIXED;
    pact->biastype = mjBIAS_AFFINE;

    if (type=="intvelocity") {
      pact->dyntype = mjDYN_INTEGRATOR;
      pact->actlimited = 1;
    }
  }

  // velocity servo
  else if (type=="velocity") {
    // clear bias
    mjuu_zerovec(pact->biasprm, mjNBIAS);

    // explicit attributes
    ReadAttr(elem, "kv", 1, pact->gainprm, text);
    pact->biasprm[2] = -pact->gainprm[0];

    // implied parameters
    pact->dyntype = mjDYN_NONE;
    pact->gaintype = mjGAIN_FIXED;
    pact->biastype = mjBIAS_AFFINE;
  }

  // damper
  else if (type=="damper") {
    // clear gain
    mjuu_zerovec(pact->gainprm, mjNGAIN);

    // explicit attributes
    ReadAttr(elem, "kv", 1, pact->gainprm+2, text);
    if (pact->gainprm[2]<0)
      throw mjXError(elem, "damping coefficient cannot be negative");
    pact->gainprm[2] = -pact->gainprm[2];

    // require nonnegative range
    ReadAttr(elem, "ctrlrange", 2, pact->ctrlrange, text);
    if (pact->ctrlrange[0]<0 || pact->ctrlrange[1]<0) {
      throw mjXError(elem, "damper control range cannot be negative");
    }

    // implied parameters
    pact->ctrllimited = 1;
    pact->dyntype = mjDYN_NONE;
    pact->gaintype = mjGAIN_AFFINE;
    pact->biastype = mjBIAS_NONE;
  }

  // cylinder
  else if (type=="cylinder") {
    // explicit attributes
    ReadAttr(elem, "timeconst", 1, pact->dynprm, text);
    ReadAttr(elem, "bias", 3, pact->biasprm, text);
    ReadAttr(elem, "area", 1, pact->gainprm, text);
    double diameter;
    if (ReadAttr(elem, "diameter", 1, &diameter, text)) {
      pact->gainprm[0] = mjPI / 4 * diameter*diameter;
    }

    // implied parameters
    pact->dyntype = mjDYN_FILTER;
    pact->gaintype = mjGAIN_FIXED;
    pact->biastype = mjBIAS_AFFINE;
  }

  // muscle
  else if (type=="muscle") {
    // set muscle defaults if same as global defaults
    if (pact->dynprm[0]==1) pact->dynprm[0] = 0.01;    // tau act
    if (pact->dynprm[1]==0) pact->dynprm[1] = 0.04;    // tau deact
    if (pact->gainprm[0]==1) pact->gainprm[0] = 0.75;  // range[0]
    if (pact->gainprm[1]==0) pact->gainprm[1] = 1.05;  // range[1]
    if (pact->gainprm[2]==0) pact->gainprm[2] = -1;    // force
    if (pact->gainprm[3]==0) pact->gainprm[3] = 200;   // scale
    if (pact->gainprm[4]==0) pact->gainprm[4] = 0.5;   // lmin
    if (pact->gainprm[5]==0) pact->gainprm[5] = 1.6;   // lmax
    if (pact->gainprm[6]==0) pact->gainprm[6] = 1.5;   // vmax
    if (pact->gainprm[7]==0) pact->gainprm[7] = 1.3;   // fpmax
    if (pact->gainprm[8]==0) pact->gainprm[8] = 1.2;   // fvmax

    // explicit attributes
    ReadAttr(elem, "timeconst", 2, pact->dynprm, text);
    ReadAttr(elem, "tausmooth", 1, pact->dynprm+2, text);
    if (pact->dynprm[2]<0)
      throw mjXError(elem, "muscle tausmooth cannot be negative");
    ReadAttr(elem, "range", 2, pact->gainprm, text);
    ReadAttr(elem, "force", 1, pact->gainprm+2, text);
    ReadAttr(elem, "scale", 1, pact->gainprm+3, text);
    ReadAttr(elem, "lmin", 1, pact->gainprm+4, text);
    ReadAttr(elem, "lmax", 1, pact->gainprm+5, text);
    ReadAttr(elem, "vmax", 1, pact->gainprm+6, text);
    ReadAttr(elem, "fpmax", 1, pact->gainprm+7, text);
    ReadAttr(elem, "fvmax", 1, pact->gainprm+8, text);

    // biasprm = gainprm
    for (int n=0; n<9; n++) {
      pact->biasprm[n] = pact->gainprm[n];
    }

    // implied parameters
    pact->dyntype = mjDYN_MUSCLE;
    pact->gaintype = mjGAIN_MUSCLE;
    pact->biastype = mjBIAS_MUSCLE;
  }

  // adhesion
  else if (type=="adhesion") {
    // explicit attributes
    ReadAttr(elem, "gain", 1, pact->gainprm, text);
    if (pact->gainprm[0]<0)
      throw mjXError(elem, "adhesion gain cannot be negative");

    // require nonnegative range
    ReadAttr(elem, "ctrlrange", 2, pact->ctrlrange, text);
    if (pact->ctrlrange[0]<0 || pact->ctrlrange[1]<0) {
      throw mjXError(elem, "adhesion control range cannot be negative");
    }

    // implied parameters
    pact->ctrllimited = 1;
    pact->gaintype = mjGAIN_FIXED;
    pact->biastype = mjBIAS_NONE;
  }

  else if (type == "plugin") {
    OnePlugin(elem, &pact->plugin);
    int n;
    if (MapValue(elem, "dyntype", &n, dyn_map, dyn_sz)) {
      pact->dyntype = (mjtDyn)n;
    }
    if (MapValue(elem, "actearly", &n, bool_map, 2)) {
      pact->actearly = (n==1);
    }
    ReadAttr(elem, "dynprm", mjNDYN, pact->dynprm, text, false, false);
  }

  else {          // SHOULD NOT OCCUR
    throw mjXError(elem, "unrecognized actuator type: %s", type.c_str());
  }

  // read userdata
  std::vector<double> userdata;
  if (ReadVector(elem, "user", userdata, text)) {
    mjm_setDouble(pact->userdata, userdata.data(), userdata.size());
  }

  // write info
  mjm_setString(pact->info,
      std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());
}



// make composite
void mjXReader::OneComposite(XMLElement* elem, mjmBody* pbody, mjCDef* def) {
  string text;
  int n;

  // create out-of-DOM element
  mjCComposite comp;

  // common properties
  ReadAttrTxt(elem, "prefix", comp.prefix);
  if (MapValue(elem, "type", &n, comp_map, mjNCOMPTYPES, true)) {
    comp.type = (mjtCompType)n;
  }
  ReadAttr(elem, "count", 3, comp.count, text, false, false);
  ReadAttr(elem, "spacing", 1, &comp.spacing, text, false);
  ReadAttr(elem, "offset", 3, comp.offset, text);
  ReadAttr(elem, "flatinertia", 1, &comp.flatinertia, text);

  // plugin
  XMLElement* eplugin = FirstChildElement(elem, "plugin");
  if (eplugin) {
    ReadAttrTxt(eplugin, "plugin", comp.plugin_name);
    ReadAttrTxt(eplugin, "instance", comp.plugin_instance_name);
    if (comp.plugin_instance_name.empty()) {
      comp.plugin_instance = (mjCPlugin*)mjm_addPlugin(model);
      comp.plugin_instance->name = "composite"+comp.prefix;
      comp.plugin_instance_name = comp.plugin_instance->name;
    } else {
      model->hasImplicitPluginElem = true;
    }
    ReadPluginConfigs(eplugin, comp.plugin_instance);
  }

  // cable
  std::string curves;
  ReadAttrTxt(elem, "curve", curves);
  ReadAttrTxt(elem, "initial", comp.initial);
  ReadAttr(elem, "size", 3, comp.size, text, false, false);
  if (ReadAttrTxt(elem, "vertex", text)){
    String2Vector(text, comp.uservert);
  }

  // shell
  ReadAttrTxt(elem, "face", comp.userface);

  // process curve string
  std::istringstream iss(curves);
  int i = 0;
  while (iss) {
    iss >> text;
    if (i>2) {
      throw mjXError(elem, "The curve array must have a maximum of 3 components");
    }
    comp.curve[i++] = (mjtCompShape)FindKey(shape_map, mjNCOMPSHAPES, text);
    if (iss.eof()){
      break;
    }
  };

  // skin
  XMLElement* eskin = FirstChildElement(elem, "skin");
  if (eskin) {
    comp.skin = true;
    if (MapValue(eskin, "texcoord", &n, bool_map, 2)) {
      comp.skintexcoord = (n==1);
    }
    ReadAttrTxt(eskin, "material", comp.skinmaterial);
    ReadAttr(eskin, "rgba", 4, comp.skinrgba, text);
    ReadAttr(eskin, "inflate", 1, &comp.skininflate, text);
    ReadAttrInt(eskin, "subgrid", &comp.skinsubgrid);
    ReadAttrInt(eskin, "group", &comp.skingroup, 0);
    if (comp.skingroup<0 || comp.skingroup>=mjNGROUP) {
      throw mjXError(eskin, "skin group must be between 0 and 5");
    }
  }

  // set type-specific defaults
  comp.SetDefault();

  // parse smooth solver parameters after type-specific defaults are set
  ReadAttr(elem, "solrefsmooth", mjNREF, comp.solrefsmooth, text, false, false);
  ReadAttr(elem, "solimpsmooth", mjNIMP, comp.solimpsmooth, text, false, false);

  // geom
  XMLElement* egeom = FirstChildElement(elem, "geom");
  if (egeom) {
    std::string material;
    mjmGeom& dgeom = comp.def[0].geom.spec;
    if (MapValue(egeom, "type", &n, geom_map, mjNGEOMTYPES)) {
      dgeom.type = (mjtGeom)n;
    }
    ReadAttr(egeom, "size", 3, dgeom.size, text, false, false);
    ReadAttrInt(egeom, "contype", &dgeom.contype);
    ReadAttrInt(egeom, "conaffinity", &dgeom.conaffinity);
    ReadAttrInt(egeom, "condim", &dgeom.condim);
    ReadAttrInt(egeom, "group", &dgeom.group);
    ReadAttrInt(egeom, "priority", &dgeom.priority);
    ReadAttr(egeom, "friction", 3, dgeom.friction, text, false, false);
    ReadAttr(egeom, "solmix", 1, &dgeom.solmix, text);
    ReadAttr(egeom, "solref", mjNREF, dgeom.solref, text, false, false);
    ReadAttr(egeom, "solimp", mjNIMP, dgeom.solimp, text, false, false);
    ReadAttr(egeom, "margin", 1, &dgeom.margin, text);
    ReadAttr(egeom, "gap", 1, &dgeom.gap, text);
    if (ReadAttrTxt(egeom, "material", material)) {
      mjm_setString(dgeom.material, material.c_str());
    }
    ReadAttr(egeom, "rgba", 4, dgeom.rgba, text);
    ReadAttr(egeom, "mass", 1, &dgeom.mass, text);
    ReadAttr(egeom, "density", 1, &dgeom.density, text);
  }

  // site
  XMLElement* esite = FirstChildElement(elem, "site");
  if (esite) {
    std::string material;
    mjmSite& dsite = comp.def[0].site.spec;
    ReadAttr(esite, "size", 3, dsite.size, text, false, false);
    ReadAttrInt(esite, "group", &dsite.group);
    ReadAttrTxt(esite, "material", material);
    ReadAttr(esite, "rgba", 4, dsite.rgba, text);
    mjm_setString(dsite.material, material.c_str());
  }

  // joint
  XMLElement* ejnt = FirstChildElement(elem, "joint");
  while (ejnt) {
    // kind
    int kind;
    MapValue(ejnt, "kind", &kind, jkind_map, 4, true);

    // create a new element if this kind already exists
    if (comp.add[kind]) {
      char error[200];
      if (!comp.AddDefaultJoint(error, 200)) {
        throw mjXError(elem, "%s", error);
      }
    }
    comp.add[kind] = true;

    // get element
    mjCDef *el = &comp.defjoint[(mjtCompKind)kind].back();

    // particle joint
    if (MapValue(ejnt, "type", &n, joint_map, joint_sz)) {
      el->joint.spec.type = (mjtJoint)n;
    }
    ReadAttr(ejnt, "axis", 3, el->joint.spec.axis, text);

    // solreffix, solimpfix
    ReadAttr(ejnt, "solreffix", mjNREF, el->equality.spec.solref, text, false, false);
    ReadAttr(ejnt, "solimpfix", mjNIMP, el->equality.spec.solimp, text, false, false);

    // joint attributes
    MapValue(elem, "limited", &el->joint.spec.limited, TFAuto_map, 3);
    ReadAttrInt(ejnt, "group", &el->joint.spec.group);
    ReadAttr(ejnt, "solreflimit", mjNREF, el->joint.spec.solref_limit, text, false, false);
    ReadAttr(ejnt, "solimplimit", mjNIMP, el->joint.spec.solimp_limit, text, false, false);
    ReadAttr(ejnt,
             "solreffriction", mjNREF, el->joint.spec.solref_friction, text, false, false);
    ReadAttr(ejnt,
             "solimpfriction", mjNIMP, el->joint.spec.solimp_friction, text, false, false);
    ReadAttr(ejnt, "stiffness", 1, &el->joint.spec.stiffness, text);
    ReadAttr(ejnt, "range", 2, el->joint.spec.range, text);
    ReadAttr(ejnt, "margin", 1, &el->joint.spec.margin, text);
    ReadAttr(ejnt, "armature", 1, &el->joint.spec.armature, text);
    ReadAttr(ejnt, "damping", 1, &el->joint.spec.damping, text);
    ReadAttr(ejnt, "frictionloss", 1, &el->joint.spec.frictionloss, text);

    // advance
    ejnt = NextSiblingElement(ejnt, "joint");
  }

  // tendon
  XMLElement* eten = FirstChildElement(elem, "tendon");
  while (eten) {
    // kind
    int kind;
    MapValue(eten, "kind", &kind, tkind_map, 2, true);
    comp.add[kind] = true;

    // solreffix, solimpfix
    ReadAttr(eten, "solreffix", mjNREF, comp.def[kind].equality.spec.solref, text, false, false);
    ReadAttr(eten, "solimpfix", mjNIMP, comp.def[kind].equality.spec.solimp, text, false, false);

    // tendon attributes
    std::string material;
    MapValue(elem, "limited", &comp.def[kind].tendon.spec.limited, TFAuto_map, 3);
    ReadAttrInt(eten, "group", &comp.def[kind].tendon.spec.group);
    ReadAttr(eten, "solreflimit", mjNREF, comp.def[kind].tendon.spec.solref_limit, text, false, false);
    ReadAttr(eten, "solimplimit", mjNIMP, comp.def[kind].tendon.spec.solimp_limit, text, false, false);
    ReadAttr(eten,
             "solreffriction", mjNREF, comp.def[kind].tendon.spec.solref_friction, text, false, false);
    ReadAttr(eten,
             "solimpfriction", mjNIMP, comp.def[kind].tendon.spec.solimp_friction, text, false, false);
    ReadAttr(eten, "range", 2, comp.def[kind].tendon.spec.range, text);
    ReadAttr(eten, "margin", 1, &comp.def[kind].tendon.spec.margin, text);
    ReadAttr(eten, "stiffness", 1, &comp.def[kind].tendon.spec.stiffness, text);
    ReadAttr(eten, "damping", 1, &comp.def[kind].tendon.spec.damping, text);
    ReadAttr(eten, "frictionloss", 1, &comp.def[kind].tendon.spec.frictionloss, text);
    ReadAttrTxt(eten, "material", material);
    mjm_setString(comp.def[kind].tendon.spec.material, material.c_str());
    ReadAttr(eten, "rgba", 4, comp.def[kind].tendon.spec.rgba, text);
    ReadAttr(eten, "width", 1, &comp.def[kind].tendon.spec.width, text);

    // advance
    eten = NextSiblingElement(eten, "tendon");
  }

  // pin
  XMLElement* epin = FirstChildElement(elem, "pin");
  while (epin) {
    // read
    int coord[2] = {0, 0};
    ReadAttr(epin, "coord", 2, coord, text, true, false);

    // insert 2 coordinates (2nd may be unused)
    comp.pin.push_back(coord[0]);
    comp.pin.push_back(coord[1]);

    // advance
    epin = NextSiblingElement(epin, "pin");
  }

  // make composite
  char error[200];
  bool res = comp.Make((mjCModel*)mjm_getModel(pbody), pbody, error, 200);

  // throw error
  if (!res) {
    throw mjXError(elem, "%s", error);
  }
}



// make flexcomp
void mjXReader::OneFlexcomp(XMLElement* elem, mjmBody* pbody) {
  string text, material;
  int n;

  // create out-of-DOM element
  mjCFlexcomp fcomp;

  // common properties
  ReadAttrTxt(elem, "name", fcomp.name, true);
  if (MapValue(elem, "type", &n, fcomp_map, mjNFCOMPTYPES)) {
    fcomp.type = (mjtFcompType)n;
  }
  ReadAttr(elem, "count", 3, fcomp.count, text);
  ReadAttr(elem, "spacing", 3, fcomp.spacing, text);
  ReadAttr(elem, "scale", 3, fcomp.scale, text);
  ReadAttr(elem, "mass", 1, &fcomp.mass, text);
  ReadAttr(elem, "inertiabox", 1, &fcomp.inertiabox, text);
  ReadAttrTxt(elem, "file", fcomp.file);
  if (ReadAttrTxt(elem, "material", material)) {
    mjm_setString(fcomp.def.flex.spec.material, material.c_str());
  }
  ReadAttr(elem, "rgba", 4, fcomp.def.flex.spec.rgba, text);
  if (MapValue(elem, "flatskin", &n, bool_map, 2)) {
    fcomp.def.flex.spec.flatskin = (n==1);
  }
  ReadAttrInt(elem, "dim", &fcomp.def.flex.spec.dim);
  ReadAttr(elem, "radius", 1, &fcomp.def.flex.spec.radius, text);
  ReadAttrInt(elem, "group", &fcomp.def.flex.spec.group);

  // pose
  ReadAttr(elem, "pos", 3, fcomp.pos, text);
  ReadAttr(elem, "quat", 4, fcomp.quat, text);
  ReadAlternative(elem, fcomp.alt);

  // user or internal
  if (MapValue(elem, "rigid", &n, bool_map, 2)) {
    fcomp.rigid = (n==1);
  }
  if (ReadAttrTxt(elem, "point", text)){
    String2Vector(text, fcomp.point);
  }
  if (ReadAttrTxt(elem, "element", text)){
    String2Vector(text, fcomp.element);
  }
  if (ReadAttrTxt(elem, "texcoord", text)) {
    String2Vector(text, fcomp.texcoord);
  }

  // edge
  XMLElement* edge = FirstChildElement(elem, "edge");
  if (edge) {
    if (MapValue(edge, "equality", &n, bool_map, 2)) {
      fcomp.equality = (n==1);
    }
    ReadAttr(edge, "solref", mjNREF, fcomp.def.equality.spec.solref, text, false, false);
    ReadAttr(edge, "solimp", mjNIMP, fcomp.def.equality.spec.solimp, text, false, false);
    ReadAttr(edge, "stiffness", 1, &fcomp.def.flex.spec.edgestiffness, text);
    ReadAttr(edge, "damping", 1, &fcomp.def.flex.spec.edgedamping, text);
  }

  // contact
  XMLElement* cont = FirstChildElement(elem, "contact");
  if (cont) {
    ReadAttrInt(cont, "contype", &fcomp.def.flex.spec.contype);
    ReadAttrInt(cont, "conaffinity", &fcomp.def.flex.spec.conaffinity);
    ReadAttrInt(cont, "condim", &fcomp.def.flex.spec.condim);
    ReadAttrInt(cont, "priority", &fcomp.def.flex.spec.priority);
    ReadAttr(cont, "friction", 3, fcomp.def.flex.spec.friction, text, false, false);
    ReadAttr(cont, "solmix", 1, &fcomp.def.flex.spec.solmix, text);
    ReadAttr(cont, "solref", mjNREF, fcomp.def.flex.spec.solref, text, false, false);
    ReadAttr(cont, "solimp", mjNIMP, fcomp.def.flex.spec.solimp, text, false, false);
    ReadAttr(cont, "margin", 1, &fcomp.def.flex.spec.margin, text);
    ReadAttr(cont, "gap", 1, &fcomp.def.flex.spec.gap, text);
    if (MapValue(cont, "internal", &n, bool_map, 2)) {
      fcomp.def.flex.spec.internal = (n==1);
    }
    MapValue(cont, "selfcollide", &fcomp.def.flex.spec.selfcollide, flexself_map, 5);
    ReadAttrInt(cont, "activelayers", &fcomp.def.flex.spec.activelayers);
  }

  // pin
  XMLElement* epin = FirstChildElement(elem, "pin");
  while (epin) {
    // accumulate id, coord, range
    vector<int> temp;
    if (ReadAttrTxt(epin, "id", text)){
      String2Vector(text, temp);
      fcomp.pinid.insert(fcomp.pinid.end(), temp.begin(), temp.end());
    }
    if (ReadAttrTxt(epin, "range", text)){
      String2Vector(text, temp);
      fcomp.pinrange.insert(fcomp.pinrange.end(), temp.begin(), temp.end());
    }
    if (ReadAttrTxt(epin, "grid", text)){
      String2Vector(text, temp);
      fcomp.pingrid.insert(fcomp.pingrid.end(), temp.begin(), temp.end());
    }
    if (ReadAttrTxt(epin, "gridrange", text)){
      String2Vector(text, temp);
      fcomp.pingridrange.insert(fcomp.pingridrange.end(), temp.begin(), temp.end());
    }

    // advance
    epin = NextSiblingElement(epin, "pin");
  }

  // plugin
  XMLElement* eplugin = FirstChildElement(elem, "plugin");
  if (eplugin) {
    ReadAttrTxt(eplugin, "plugin", fcomp.plugin_name);
    ReadAttrTxt(eplugin, "instance", fcomp.plugin_instance_name);
    if (fcomp.plugin_instance_name.empty()) {
      fcomp.plugin_instance = (mjCPlugin*)mjm_addPlugin(model);
      fcomp.plugin_instance->name = "flexcomp_" + fcomp.name;
      fcomp.plugin_instance_name = fcomp.plugin_instance->name;
    } else {
      model->hasImplicitPluginElem = true;
    }
    ReadPluginConfigs(eplugin, fcomp.plugin_instance);
  }

  // make flexcomp
  char error[200];
  bool res = fcomp.Make((mjCModel*)mjm_getModel(pbody), pbody, error, 200);

  // throw error
  if (!res) {
    throw mjXError(elem, "%s", error);
  }
}



// add plugin
void mjXReader::OnePlugin(XMLElement* elem, mjmPlugin* plugin) {
  plugin->active = true;
  std::string name = "";
  std::string instance_name = "";
  ReadAttrTxt(elem, "plugin", name);
  ReadAttrTxt(elem, "instance", instance_name);
  mjm_setString(plugin->name, name.c_str());
  mjm_setString(plugin->instance_name, instance_name.c_str());
  if (instance_name.empty()) {
    plugin->instance = mjm_addPlugin(model);
    ReadPluginConfigs(elem, (mjCPlugin*)plugin->instance);
  } else {
    model->hasImplicitPluginElem = true;
  }
}



//------------------ MJCF-specific sections --------------------------------------------------------

// default section parser
void mjXReader::Default(XMLElement* section, int parentid) {
  XMLElement* elem;
  string text, name;
  mjCDef* def;
  int thisid;

  // create new default, except at top level (already added in mjCModel ctor)
  text.clear();
  ReadAttrTxt(section, "class", text);
  if (text.empty()) {
    if (parentid>=0) {
      throw mjXError(section, "empty class name");
    } else {
      text = "main";
    }
  }
  if (parentid>=0) {
    thisid = (int)model->defaults.size();
    def = model->AddDef(text, parentid);
    if (!def) {
      throw mjXError(section, "repeated default class name");
    }
  } else {
    thisid = 0;
    def = model->defaults[0];
    def->name = text;
  }

  // iterate over elements other than nested defaults
  elem = FirstChildElement(section);
  while (elem) {
    // get element name
    name = elem->Value();

    // read mesh
    if (name=="mesh") OneMesh(elem, &def->mesh.spec);

    // read material
    else if (name=="material") OneMaterial(elem, &def->material.spec);

    // read joint
    else if (name=="joint") OneJoint(elem, &def->joint.spec);

    // read geom
    else if (name=="geom") OneGeom(elem, &def->geom.spec);

    // read site
    else if (name=="site") OneSite(elem, def->site.spec);

    // read camera
    else if (name=="camera") OneCamera(elem, &def->camera.spec);

    // read light
    else if (name=="light") OneLight(elem, &def->light.spec);

    // read pair
    else if (name=="pair") OnePair(elem, &def->pair.spec);

    // read equality
    else if (name=="equality") OneEquality(elem, &def->equality.spec);

    // read tendon
    else if (name=="tendon") OneTendon(elem, &def->tendon.spec);

    // read actuator
    else if (name=="general"     ||
             name=="motor"       ||
             name=="position"    ||
             name=="velocity"    ||
             name=="damper"      ||
             name=="intvelocity" ||
             name=="cylinder"    ||
             name=="muscle"      ||
             name=="adhesion") {
      OneActuator(elem, &def->actuator.spec);
    }

    // copy into private attributes
    mjm_finalize(def->geom.spec.element);
    mjm_finalize(def->joint.spec.element);
    mjm_finalize(def->site.spec.element);
    mjm_finalize(def->camera.spec.element);
    mjm_finalize(def->light.spec.element);
    mjm_finalize(def->actuator.spec.element);
    mjm_finalize(def->material.spec.element);
    mjm_finalize(def->equality.spec.element);
    mjm_finalize(def->tendon.spec.element);
    mjm_finalize(def->flex.spec.element);
    mjm_finalize(def->pair.spec.element);

    // advance
    elem = NextSiblingElement(elem);
  }

  // iterate over nested defaults
  elem = FirstChildElement(section);
  while (elem) {
    // get element name
    name = elem->Value();

    // read default
    if (name=="default") {
      Default(elem, thisid);
    }

    // advance
    elem = NextSiblingElement(elem);
  }
}



// extension section parser
void mjXReader::Extension(XMLElement* section) {
  XMLElement* elem = FirstChildElement(section);
  while (elem) {
    // get sub-element name
    std::string_view name = elem->Value();

    if (name == "plugin") {
      std::string plugin_name;
      int plugin_slot = -1;
      ReadAttrTxt(elem, "plugin", plugin_name, /* required = */ true);
      const mjpPlugin* plugin = mjp_getPlugin(plugin_name.c_str(), &plugin_slot);
      if (!plugin) {
        throw mjXError(elem, "unknown plugin '%s'", plugin_name.c_str());
      }

      bool already_declared = false;
      for (const auto& [existing_plugin, existing_slot] : model->active_plugins) {
        if (plugin == existing_plugin) {
          already_declared = true;
          break;
        }
      }
      if (!already_declared) {
        model->active_plugins.emplace_back(std::make_pair(plugin, plugin_slot));
      }

      XMLElement* child = FirstChildElement(elem);
      while (child) {
        if (std::string(child->Value())=="instance") {
          if (model->hasImplicitPluginElem) {
            throw mjXError(
                child, "explicit plugin instance must appear before implicit plugin elements");
          }
          mjCPlugin* pp = (mjCPlugin*)mjm_addPlugin(model);
          GetXMLPos(child, pp);
          ReadAttrTxt(child, "name", pp->name, /* required = */ true);
          if (pp->name.empty()) {
            throw mjXError(child, "plugin instance must have a name");
          }
          ReadPluginConfigs(child, pp);
          pp->plugin_slot = plugin_slot;
          pp->nstate = -1;  // actual value to be filled in by the plugin later
        }
        child = NextSiblingElement(child);
      }
    }

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// custom section parser
void mjXReader::Custom(XMLElement* section) {
  string text, name;
  XMLElement* elem;
  double data[500];

  // iterate over child elements
  elem = FirstChildElement(section);
  while (elem) {
    // get sub-element name
    name = elem->Value();
    string elname;

    // numeric
    if (name=="numeric") {
      // create custom
      mjmNumeric* pnum = mjm_addNumeric(model);

      // write error info
      mjm_setString(pnum->info,
          std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());

      // read attributes
      ReadAttrTxt(elem, "name", elname, true);
      mjm_setString(pnum->name, elname.c_str());
      if (ReadAttrInt(elem, "size", &pnum->size)) {
        int sz = pnum->size < 500 ? pnum->size : 500;
        for (int i=0; i<sz; i++) {
          data[i] = 0;
        }
      } else {
        pnum->size = 501;
      }
      int len = ReadAttr(elem, "data", pnum->size, data, text, false, false);
      if (pnum->size==501) {
        pnum->size = len;
      }
      if (pnum->size<1 || pnum->size>500) {
        throw mjXError(elem, "custom field size must be between 1 and 500");
      }

      // copy data
      mjm_setDouble(pnum->data, data, pnum->size);
    }

    // text
    else if (name=="text") {
      // create custom
      mjmText* pte = mjm_addText(model);

      // write error info
      mjm_setString(pte->info,
          std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());

      // read attributes
      ReadAttrTxt(elem, "name", elname, true);
      mjm_setString(pte->name, elname.c_str());
      ReadAttrTxt(elem, "data", text, true);
      if (text.empty()) {
        throw mjXError(elem, "text field cannot be empty");
      }

      // copy data
      mjm_setString(pte->data, text.c_str());
    }

    // tuple
    else if (name=="tuple") {
      // create custom
      mjmTuple* ptu = mjm_addTuple(model);

      // write error info
      mjm_setString(ptu->info,
          std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());

      // read attributes
      ReadAttrTxt(elem, "name", elname, true);
      mjm_setString(ptu->name, elname.c_str());

      // read objects and add
      XMLElement* obj = FirstChildElement(elem);
      std::vector<int> objtype;
      std::string objname = "";
      std::vector<double> objprm;

      while (obj) {
        // get sub-element name
        name = obj->Value();

        // new object
        if (name=="element") {
          // read type, check and assign
          ReadAttrTxt(obj, "objtype", text, true);
          mjtObj otype = (mjtObj)mju_str2Type(text.c_str());
          if (otype==mjOBJ_UNKNOWN) {
            throw mjXError(obj, "unknown object type");
          }
          objtype.push_back(otype);

          // read name and assign
          ReadAttrTxt(obj, "objname", text, true);
          objname += text + " ";

          // read parameter and assign
          double oprm = 0;
          ReadAttr(obj, "prm", 1, &oprm, text);
          objprm.push_back(oprm);
        }

        // advance to next object
        obj = NextSiblingElement(obj);
      }

      mjm_setInt(ptu->objtype, objtype.data(), objtype.size());
      mjm_setStringVec(ptu->objname, objname.c_str());
      mjm_setDouble(ptu->objprm, objprm.data(), objprm.size());
    }

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// visual section parser
void mjXReader::Visual(XMLElement* section) {
  string text, name;
  XMLElement* elem;
  mjVisual* vis = &model->visual;

  // iterate over child elements
  elem = FirstChildElement(section);
  while (elem) {
    // get sub-element name
    name = elem->Value();

    // global sub-element
    if (name=="global") {
      ReadAttr(elem,    "fovy",      1, &vis->global.fovy,      text);
      ReadAttr(elem,    "ipd",       1, &vis->global.ipd,       text);
      ReadAttr(elem,    "azimuth",   1, &vis->global.azimuth,   text);
      ReadAttr(elem,    "elevation", 1, &vis->global.elevation, text);
      ReadAttr(elem,    "linewidth", 1, &vis->global.linewidth, text);
      ReadAttr(elem,    "glow",      1, &vis->global.glow,      text);
      ReadAttrInt(elem, "offwidth",     &vis->global.offwidth);
      ReadAttrInt(elem, "offheight",    &vis->global.offheight);
      if (ReadAttr(elem, "realtime", 1, &vis->global.realtime, text)) {
        if (vis->global.realtime<=0) {
          throw mjXError(elem, "realtime must be greater than 0");
        }
      }
      int ellipsoidinertia;
      if (MapValue(elem, "ellipsoidinertia", &ellipsoidinertia, bool_map, 2)) {
        vis->global.ellipsoidinertia = (ellipsoidinertia==1);
      }
      int bvactive;
      if (MapValue(elem, "bvactive", &bvactive, bool_map, 2)) {
        vis->global.bvactive = (bvactive==1);
      }
    }

    // quality sub-element
    else if (name=="quality") {
      ReadAttrInt(elem, "shadowsize", &vis->quality.shadowsize);
      ReadAttrInt(elem, "offsamples", &vis->quality.offsamples);
      ReadAttrInt(elem, "numslices",  &vis->quality.numslices);
      ReadAttrInt(elem, "numstacks",  &vis->quality.numstacks);
      ReadAttrInt(elem, "numquads",   &vis->quality.numquads);
    }

    // headlight sub-element
    else if (name=="headlight") {
      ReadAttr(elem, "ambient",  3, vis->headlight.ambient,  text);
      ReadAttr(elem, "diffuse",  3, vis->headlight.diffuse,  text);
      ReadAttr(elem, "specular", 3, vis->headlight.specular, text);
      ReadAttrInt(elem, "active",  &vis->headlight.active);
    }

    // map sub-element
    else if (name=="map") {
      ReadAttr(elem, "stiffness",      1, &vis->map.stiffness, text);
      ReadAttr(elem, "stiffnessrot",   1, &vis->map.stiffnessrot, text);
      ReadAttr(elem, "force",          1, &vis->map.force,     text);
      ReadAttr(elem, "torque",         1, &vis->map.torque,    text);
      ReadAttr(elem, "alpha",          1, &vis->map.alpha,     text);
      ReadAttr(elem, "fogstart",       1, &vis->map.fogstart,  text);
      ReadAttr(elem, "fogend",         1, &vis->map.fogend,    text);
      ReadAttr(elem, "znear",          1, &vis->map.znear,     text);
      if (vis->map.znear<=0) {
        throw mjXError(elem, "znear must be strictly positive");
      }
      ReadAttr(elem, "zfar",           1, &vis->map.zfar,      text);
      ReadAttr(elem, "haze",           1, &vis->map.haze,      text);
      ReadAttr(elem, "shadowclip",     1, &vis->map.shadowclip, text);
      ReadAttr(elem, "shadowscale",    1, &vis->map.shadowscale, text);
      ReadAttr(elem, "actuatortendon", 1, &vis->map.actuatortendon, text);
    }

    // scale sub-element
    else if (name=="scale") {
      ReadAttr(elem, "forcewidth",     1, &vis->scale.forcewidth,     text);
      ReadAttr(elem, "contactwidth",   1, &vis->scale.contactwidth,   text);
      ReadAttr(elem, "contactheight",  1, &vis->scale.contactheight,  text);
      ReadAttr(elem, "connect",        1, &vis->scale.connect,        text);
      ReadAttr(elem, "com",            1, &vis->scale.com,            text);
      ReadAttr(elem, "camera",         1, &vis->scale.camera,         text);
      ReadAttr(elem, "light",          1, &vis->scale.light,          text);
      ReadAttr(elem, "selectpoint",    1, &vis->scale.selectpoint,    text);
      ReadAttr(elem, "jointlength",    1, &vis->scale.jointlength,    text);
      ReadAttr(elem, "jointwidth",     1, &vis->scale.jointwidth,     text);
      ReadAttr(elem, "actuatorlength", 1, &vis->scale.actuatorlength, text);
      ReadAttr(elem, "actuatorwidth",  1, &vis->scale.actuatorwidth,  text);
      ReadAttr(elem, "framelength",    1, &vis->scale.framelength,    text);
      ReadAttr(elem, "framewidth",     1, &vis->scale.framewidth,     text);
      ReadAttr(elem, "constraint",     1, &vis->scale.constraint,     text);
      ReadAttr(elem, "slidercrank",    1, &vis->scale.slidercrank,    text);
      ReadAttr(elem, "frustum",        1, &vis->scale.frustum,        text);
    }

    // rgba sub-element
    else if (name=="rgba") {
      ReadAttr(elem, "fog",              4, vis->rgba.fog,             text);
      ReadAttr(elem, "haze",             4, vis->rgba.haze,            text);
      ReadAttr(elem, "force",            4, vis->rgba.force,           text);
      ReadAttr(elem, "inertia",          4, vis->rgba.inertia,         text);
      ReadAttr(elem, "joint",            4, vis->rgba.joint,           text);
      ReadAttr(elem, "actuator",         4, vis->rgba.actuator,        text);
      ReadAttr(elem, "actuatornegative", 4, vis->rgba.actuatornegative, text);
      ReadAttr(elem, "actuatorpositive", 4, vis->rgba.actuatorpositive, text);
      ReadAttr(elem, "com",              4, vis->rgba.com,             text);
      ReadAttr(elem, "camera",           4, vis->rgba.camera,          text);
      ReadAttr(elem, "light",            4, vis->rgba.light,           text);
      ReadAttr(elem, "selectpoint",      4, vis->rgba.selectpoint,     text);
      ReadAttr(elem, "connect",          4, vis->rgba.connect,         text);
      ReadAttr(elem, "contactpoint",     4, vis->rgba.contactpoint,    text);
      ReadAttr(elem, "contactforce",     4, vis->rgba.contactforce,    text);
      ReadAttr(elem, "contactfriction",  4, vis->rgba.contactfriction, text);
      ReadAttr(elem, "contacttorque",    4, vis->rgba.contacttorque,   text);
      ReadAttr(elem, "contactgap",       4, vis->rgba.contactgap,      text);
      ReadAttr(elem, "rangefinder",      4, vis->rgba.rangefinder,     text);
      ReadAttr(elem, "constraint",       4, vis->rgba.constraint,      text);
      ReadAttr(elem, "slidercrank",      4, vis->rgba.slidercrank,     text);
      ReadAttr(elem, "crankbroken",      4, vis->rgba.crankbroken,     text);
      ReadAttr(elem, "frustum",          4, vis->rgba.frustum,         text);
      ReadAttr(elem, "bv",               4, vis->rgba.bv,              text);
      ReadAttr(elem, "bvactive",         4, vis->rgba.bvactive,        text);
    }

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// asset section parser
void mjXReader::Asset(XMLElement* section) {
  int n;
  string text, name, texname, content_type, file;
  XMLElement* elem;

  // iterate over child elements
  elem = FirstChildElement(section);
  while (elem) {
    // get sub-element name
    name = elem->Value();

    // get class if specified, otherwise use default0
    mjCDef* def = GetClass(elem);
    if (!def) {
      def = model->defaults[0];
    }

    // texture sub-element
    if (name=="texture") {
      // create texture
      mjmTexture* ptex = mjm_addTexture(model);

      // write error info
      mjm_setString(ptex->info,
          std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());

      // read attributes
      if (MapValue(elem, "type", &n, texture_map, texture_sz)) {
        ptex->type = (mjtTexture)n;
      }
      if (ReadAttrTxt(elem, "name", texname)) {
        mjm_setString(ptex->name, texname.c_str());
      }
      if (ReadAttrTxt(elem, "content_type", content_type)) {
        mjm_setString(ptex->content_type, content_type.c_str());
      }
      if (ReadAttrTxt(elem, "file", file)) {
        mjm_setString(ptex->file, file.c_str());
      }
      ReadAttrInt(elem, "width", &ptex->width);
      ReadAttrInt(elem, "height", &ptex->height);
      ReadAttr(elem, "rgb1", 3, ptex->rgb1, text);
      ReadAttr(elem, "rgb2", 3, ptex->rgb2, text);
      ReadAttr(elem, "markrgb", 3, ptex->markrgb, text);
      ReadAttr(elem, "random", 1, &ptex->random, text);
      if (MapValue(elem, "builtin", &n, builtin_map, builtin_sz)) {
        ptex->builtin = (mjtBuiltin)n;
      }
      if (MapValue(elem, "mark", &n, mark_map, mark_sz)) {
        ptex->mark = (mjtMark)n;
      }
      if (MapValue(elem, "hflip", &n, bool_map, 2)) {
        ptex->hflip = (n!=0);
      }
      if (MapValue(elem, "vflip", &n, bool_map, 2)) {
        ptex->vflip = (n!=0);
      }

      // grid
      ReadAttr(elem, "gridsize", 2, ptex->gridsize, text);
      if (ReadAttrTxt(elem, "gridlayout", text)) {
        // check length
        if (text.length()>12) {
          throw mjXError(elem, "gridlayout length cannot exceed 12 characters");
        }
        if (text.length()!=ptex->gridsize[0]*ptex->gridsize[1]) {
          throw mjXError(elem, "gridlayout length must match gridsize");
        }

        memcpy(ptex->gridlayout, text.data(), text.length());
      }

      // separate files
      std::vector<string> cubefiles(6);
      ReadAttrTxt(elem, "fileright", cubefiles[0]);
      ReadAttrTxt(elem, "fileleft",  cubefiles[1]);
      ReadAttrTxt(elem, "fileup",    cubefiles[2]);
      ReadAttrTxt(elem, "filedown",  cubefiles[3]);
      ReadAttrTxt(elem, "filefront", cubefiles[4]);
      ReadAttrTxt(elem, "fileback",  cubefiles[5]);
      for (int i = 0; i < cubefiles.size(); i++) {
        mjm_setInStringVec(ptex->cubefiles, i, cubefiles[i].c_str());
      }
    }

    // material sub-element
    else if (name=="material") {
      // create material and parse
      mjmMaterial* pmat = mjm_addMaterial(model, def);
      OneMaterial(elem, pmat);
    }

    // mesh sub-element
    else if (name=="mesh") {
      // create mesh and parse
      mjmMesh* pmesh = mjm_addMesh(model, def);
      OneMesh(elem, pmesh);
    }

    // skin sub-element... deprecate ???
    else if (name=="skin") {
      // create skin and parse
      mjCSkin* pskin = model->AddSkin();
      OneSkin(elem, pskin);
    }

    // hfield sub-element
    else if (name=="hfield") {
      // create hfield
      mjmHField* phf = mjm_addHField(model);

      // write error info
      mjm_setString(phf->info,
          std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());

      // read attributes
      string name, content_type, file;
      if (ReadAttrTxt(elem, "name", name)) {
        mjm_setString(phf->name, name.c_str());
      }
      if (ReadAttrTxt(elem, "content_type", content_type)) {
        mjm_setString(phf->content_type, content_type.c_str());
      }
      if (ReadAttrTxt(elem, "file", file)) {
        mjm_setString(phf->file, file.c_str());
      }
      ReadAttrInt(elem, "nrow", &phf->nrow);
      ReadAttrInt(elem, "ncol", &phf->ncol);
      ReadAttr(elem, "size", 4, phf->size, text, true);

      // allocate buffer for dynamic hfield, copy user data if given
      if (file.empty() && phf->nrow>0 && phf->ncol>0) {
        int nrow = phf->nrow;
        int ncol = phf->ncol;

        // read user data
        auto userdata = ReadAttrVec<float>(elem, "elevation");

        // user data given, copy into data
        if (userdata.has_value()) {
          if (userdata->size() != nrow*ncol) {
            throw mjXError(elem, "elevation data length must match nrow*ncol");
          }

          // copy in reverse row order, so XML string is top-to-bottom
          std::vector<float> flipped(nrow*ncol);
          for (int i = 0; i < nrow; i++) {
            int flip = nrow-1-i;
            for (int j = 0; j < ncol; j++) {
              flipped[flip*ncol + j] = userdata->data()[i*ncol + j];
            }
          }

          mjm_setFloat(phf->userdata, flipped.data(), flipped.size());
        }

        // user data not given, set to 0
        else {
          std::vector<float> zero(nrow*ncol);
          mjm_setFloat(phf->userdata, zero.data(), zero.size());
        }
      }
    }

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// body/world section parser; recursive
void mjXReader::Body(XMLElement* section, mjmBody* pbody, mjmFrame* frame) {
  string text, name;
  XMLElement* elem;
  int n;

  // sanity check
  if (!pbody) {
    throw mjXError(section, "null body pointer");
  }

  // no attributes allowed in world body
  if (mjm_getId(pbody->element)==0 && section->FirstAttribute() && !frame) {
    throw mjXError(section, "World body cannot have attributes");
  }

  // iterate over sub-elements; attributes set while parsing parent body
  elem = FirstChildElement(section);
  while (elem) {
    // get sub-element name
    name = elem->Value();

    // get class if specified, otherwise use body
    mjCDef* def = GetClass(elem);
    if (!def) {
      def = (mjCDef*)mjm_getDefault(pbody->element);
    }

    // inertial sub-element
    if (name=="inertial") {
      // no inertia allowed in world body
      if (mjm_getId(pbody->element)==0) {
        throw mjXError(elem, "World body cannot have inertia");
      }
      pbody->explicitinertial = true;
      ReadAttr(elem, "pos", 3, pbody->ipos, text, true);
      ReadQuat(elem, "quat", pbody->iquat, text);
      ReadAttr(elem, "mass", 1, &pbody->mass, text, true);
      ReadAttr(elem, "diaginertia", 3, pbody->inertia, text);
      bool alt = ReadAlternative(elem, pbody->ialt);
      bool full = ReadAttr(elem, "fullinertia", 6, pbody->fullinertia, text);
      if (alt && full) {
        throw mjXError(elem, "multiple orientation specifiers are not allowed");
      }
    }

    // joint sub-element
    else if (name=="joint") {
      // no joints allowed in world body
      if (mjm_getId(pbody->element)==0) {
        throw mjXError(elem, "World body cannot have joints");
      }

      // create joint and parse
      mjmJoint* pjoint = mjm_addJoint(pbody, def);
      OneJoint(elem, pjoint);
      mjm_setFrame(pjoint->element, frame);
    }

    // freejoint sub-element
    else if (name=="freejoint") {
      // no joints allowed in world body
      if (mjm_getId(pbody->element)==0) {
        throw mjXError(elem, "World body cannot have joints");
      }

      // create free joint without defaults
      mjmJoint* pjoint = mjm_addFreeJoint(pbody);
      mjm_setFrame(pjoint->element, frame);

      // save defaults after creation, to make sure writing is ok
      mjm_setDefault(pjoint->element, def);

      // read attributes
      std::string name;
      if (ReadAttrTxt(elem, "name", name)) {
        mjm_setString(pjoint->name, name.c_str());
      }
      ReadAttrInt(elem, "group", &pjoint->group);
    }

    // geom sub-element
    else if (name=="geom") {
      // create geom and parse
      mjmGeom* pgeom = mjm_addGeom(pbody, def);
      OneGeom(elem, pgeom);
      mjm_setFrame(pgeom->element, frame);
    }

    // site sub-element
    else if (name=="site") {
      // create site and parse
      mjmSite* site = mjm_addSite(pbody,  def);
      OneSite(elem, *site);
      mjm_setFrame(site->element, frame);
    }

    // camera sub-element
    else if (name=="camera") {
      // create camera and parse
      mjmCamera* pcam = mjm_addCamera(pbody, def);
      OneCamera(elem, pcam);
      mjm_setFrame(pcam->element, frame);
    }

    // light sub-element
    else if (name=="light") {
      // create light and parse
      mjmLight* plight = mjm_addLight(pbody, def);
      OneLight(elem, plight);
      mjm_setFrame(plight->element, frame);
    }

    // plugin sub-element
    else if (name == "plugin") {
      OnePlugin(elem, &(pbody->plugin));
    }

    // composite sub-element
    else if (name=="composite") {
      // parse composite
      OneComposite(elem, pbody, def);
    }

    // flexcomp sub-element
    else if (name=="flexcomp") {
      // parse flexcomp
      OneFlexcomp(elem, pbody);
    }

    // frame sub-element
    else if (name=="frame") {
      mjmFrame* pframe = mjm_addFrame(pbody, frame);
      mjm_setString(pframe->info, ("line = " + std::to_string(elem->GetLineNum())).c_str());

      ReadAttr(elem, "pos", 3, pframe->pos, text);
      ReadQuat(elem, "quat", pframe->quat, text);
      ReadAlternative(elem, pframe->alt);

      Body(elem, pbody, pframe);
    }

    // body sub-element
    else if (name=="body") {
      // read childdef
      mjCDef* childdef = 0;
      if (ReadAttrTxt(elem, "childclass", text)) {
        childdef = model->FindDef(text);
        if (!childdef) {
          throw mjXError(elem, "unknown default childclass");
        }
      }

      // create child body
      mjmBody* pchild = mjm_addBody(pbody, childdef);
      mjm_setString(pchild->info,
                    std::string("line = " + std::to_string(elem->GetLineNum())).c_str());

      // read attributes
      std::string name, childclass;
      if (ReadAttrTxt(elem, "name", name)) {
        mjm_setString(pchild->name, name.c_str());
      }
      if (ReadAttrTxt(elem, "childclass", childclass)) {
        mjm_setString(pchild->classname, childclass.c_str());
      }
      ReadAttr(elem, "pos", 3, pchild->pos, text);
      ReadQuat(elem, "quat", pchild->quat, text);
      if (MapValue(elem, "mocap", &n, bool_map, 2)) {
        pchild->mocap = (n==1);
      }
      ReadAlternative(elem, pchild->alt);

      // read gravcomp
      ReadAttr(elem, "gravcomp", 1, &pchild->gravcomp, text);

      // read userdata
      std::vector<double> userdata;
      ReadVector(elem, "user", userdata, text);
      mjm_setDouble(pchild->userdata, userdata.data(), userdata.size());

      // add frame
      mjm_setFrame(pchild->element, frame);

      // make recursive call
      Body(elem, pchild, nullptr);
    }

    // no match
    else {
      throw mjXError(elem, "unrecognized model element '%s'", name.c_str());
    }

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// contact section parser
void mjXReader::Contact(XMLElement* section) {
  string text, name;
  XMLElement* elem;

  // iterate over child elements
  elem = FirstChildElement(section);
  while (elem) {
    // get sub-element name
    name = elem->Value();

    // get class if specified, otherwise use default0
    mjCDef* def = GetClass(elem);
    if (!def) {
      def = model->defaults[0];
    }

    // geom pair to include
    if (name=="pair") {
      // create pair and parse
      mjmPair* ppair = mjm_addPair(model, def);
      OnePair(elem, ppair);
    }

    // body pair to exclude
    else if (name=="exclude") {
      mjmExclude* pexclude = mjm_addExclude(model);
      string exname, exbody1, exbody2;

      // write error info
      mjm_setString(pexclude->info, ("line = " + std::to_string(elem->GetLineNum())).c_str());

      // read name and body names
      if (ReadAttrTxt(elem, "name", exname)) {
        mjm_setString(pexclude->name, exname.c_str());
      }
      ReadAttrTxt(elem, "body1", exbody1, true);
      mjm_setString(pexclude->bodyname1, exbody1.c_str());
      ReadAttrTxt(elem, "body2", exbody2, true);
      mjm_setString(pexclude->bodyname2, exbody2.c_str());
    }

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// constraint section parser
void mjXReader::Equality(XMLElement* section) {
  XMLElement* elem;

  // iterate over child elements
  elem = FirstChildElement(section);
  while (elem) {
    // get class if specified, otherwise use default0
    mjCDef* def = GetClass(elem);
    if (!def) {
      def = model->defaults[0];
    }

    // create equality constraint and parse
    mjmEquality* pequality = mjm_addEquality(model, def);
    OneEquality(elem, pequality);

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// deformable section parser
void mjXReader::Deformable(XMLElement* section) {
  string name;
  XMLElement* elem;

  // iterate over child elements
  elem = FirstChildElement(section);
  while (elem) {
    // get sub-element name
    name = elem->Value();

    // get class if specified, otherwise use default0
    mjCDef* def = GetClass(elem);
    if (!def) {
      def = model->defaults[0];
    }

    // flex sub-element
    if (name=="flex") {
      // create flex and parse
      mjmFlex* pflex = mjm_addFlex(model);
      OneFlex(elem, pflex);
    }

    // skin sub-element
    else if (name=="skin") {
      // create skin and parse
      mjCSkin* pskin = model->AddSkin();
      OneSkin(elem, pskin);
    }

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// tendon section parser
void mjXReader::Tendon(XMLElement* section) {
  string text, text1;
  XMLElement* elem;
  double data;

  // iterate over child elements
  elem = FirstChildElement(section);
  while (elem) {
    // get class if specified, otherwise use default0
    mjCDef* def = GetClass(elem);
    if (!def) {
      def = model->defaults[0];
    }

    // create equality constraint and parse
    mjmTendon* pten = mjm_addTendon(model, def);
    OneTendon(elem, pten);

    // process wrap sub-elements
    XMLElement* sub = FirstChildElement(elem);
    while (sub) {
      // get wrap type
      string wrap = sub->Value();
      mjmWrap* pwrap;;

      // read attributes depending on type
      if (wrap=="site") {
        ReadAttrTxt(sub, "site", text, true);
        pwrap = mjm_wrapSite(pten, text.c_str());
      }

      else if (wrap=="geom") {
        ReadAttrTxt(sub, "geom", text, true);
        if (!ReadAttrTxt(sub, "sidesite", text1)) {
          text1.clear();
        }
        pwrap = mjm_wrapGeom(pten, text.c_str(), text1.c_str());
      }

      else if (wrap=="pulley") {
        ReadAttr(sub, "divisor", 1, &data, text, true);
        pwrap = mjm_wrapPulley(pten, data);
      }

      else if (wrap=="joint") {
        ReadAttrTxt(sub, "joint", text, true);
        ReadAttr(sub, "coef", 1, &data, text1, true);
        pwrap = mjm_wrapJoint(pten, text.c_str(), data);
      }

      else {
        throw mjXError(sub, "unknown wrap type");  // SHOULD NOT OCCUR
      }

      mjm_setString(pwrap->info, ("line = " + std::to_string(sub->GetLineNum())).c_str());

      // advance to next sub-element
      sub = NextSiblingElement(sub);
    }

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// actuator section parser
void mjXReader::Actuator(XMLElement* section) {
  XMLElement* elem;

  // iterate over child elements
  elem = FirstChildElement(section);
  while (elem) {
    // get class if specified, otherwise use default0
    mjCDef* def = GetClass(elem);
    if (!def) {
      def = model->defaults[0];
    }

    // create actuator and parse
    mjmActuator* pact = mjm_addActuator(model, def);
    OneActuator(elem, pact);

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// sensor section parser
void mjXReader::Sensor(XMLElement* section) {
  int n;
  XMLElement* elem = FirstChildElement(section);
  while (elem) {
    // create sensor, get string type
    mjmSensor* psen = mjm_addSensor(model);
    string type = elem->Value();
    string text, name, objname, refname;
    std::vector<double> userdata;

    // read name, noise, userdata
    if (ReadAttrTxt(elem, "name", name)) {
      mjm_setString(psen->name, name.c_str());
    }
    ReadAttr(elem, "cutoff", 1, &psen->cutoff, text);
    ReadAttr(elem, "noise", 1, &psen->noise, text);
    if (ReadVector(elem, "user", userdata, text)) {
      mjm_setDouble(psen->userdata, userdata.data(), userdata.size());
    }

    // common robotic sensors, attached to a site
    if (type=="touch") {
      psen->type = mjSENS_TOUCH;
      psen->objtype = mjOBJ_SITE;
      ReadAttrTxt(elem, "site", objname, true);
    } else if (type=="accelerometer") {
      psen->type = mjSENS_ACCELEROMETER;
      psen->objtype = mjOBJ_SITE;
      ReadAttrTxt(elem, "site", objname, true);
    } else if (type=="velocimeter") {
      psen->type = mjSENS_VELOCIMETER;
      psen->objtype = mjOBJ_SITE;
      ReadAttrTxt(elem, "site", objname, true);
    } else if (type=="gyro") {
      psen->type = mjSENS_GYRO;
      psen->objtype = mjOBJ_SITE;
      ReadAttrTxt(elem, "site", objname, true);
    } else if (type=="force") {
      psen->type = mjSENS_FORCE;
      psen->objtype = mjOBJ_SITE;
      ReadAttrTxt(elem, "site", objname, true);
    } else if (type=="torque") {
      psen->type = mjSENS_TORQUE;
      psen->objtype = mjOBJ_SITE;
      ReadAttrTxt(elem, "site", objname, true);
    } else if (type=="magnetometer") {
      psen->type = mjSENS_MAGNETOMETER;
      psen->objtype = mjOBJ_SITE;
      ReadAttrTxt(elem, "site", objname, true);
    } else if (type=="camprojection") {
      psen->type = mjSENS_CAMPROJECTION;
      psen->objtype = mjOBJ_SITE;
      ReadAttrTxt(elem, "site", objname, true);
      ReadAttrTxt(elem, "camera", refname, true);
      psen->reftype = mjOBJ_CAMERA;
    } else if (type=="rangefinder") {
      psen->type = mjSENS_RANGEFINDER;
      psen->objtype = mjOBJ_SITE;
      ReadAttrTxt(elem, "site", objname, true);
    }

    // sensors related to scalar joints, tendons, actuators
    else if (type=="jointpos") {
      psen->type = mjSENS_JOINTPOS;
      psen->objtype = mjOBJ_JOINT;
      ReadAttrTxt(elem, "joint", objname, true);
    } else if (type=="jointvel") {
      psen->type = mjSENS_JOINTVEL;
      psen->objtype = mjOBJ_JOINT;
      ReadAttrTxt(elem, "joint", objname, true);
    } else if (type=="tendonpos") {
      psen->type = mjSENS_TENDONPOS;
      psen->objtype = mjOBJ_TENDON;
      ReadAttrTxt(elem, "tendon", objname, true);
    } else if (type=="tendonvel") {
      psen->type = mjSENS_TENDONVEL;
      psen->objtype = mjOBJ_TENDON;
      ReadAttrTxt(elem, "tendon", objname, true);
    } else if (type=="actuatorpos") {
      psen->type = mjSENS_ACTUATORPOS;
      psen->objtype = mjOBJ_ACTUATOR;
      ReadAttrTxt(elem, "actuator", objname, true);
    } else if (type=="actuatorvel") {
      psen->type = mjSENS_ACTUATORVEL;
      psen->objtype = mjOBJ_ACTUATOR;
      ReadAttrTxt(elem, "actuator", objname, true);
    } else if (type=="actuatorfrc") {
      psen->type = mjSENS_ACTUATORFRC;
      psen->objtype = mjOBJ_ACTUATOR;
      ReadAttrTxt(elem, "actuator", objname, true);
    } else if (type=="jointactuatorfrc") {
      psen->type = mjSENS_JOINTACTFRC;
      psen->objtype = mjOBJ_JOINT;
      ReadAttrTxt(elem, "joint", objname, true);
    }

    // sensors related to ball joints
    else if (type=="ballquat") {
      psen->type = mjSENS_BALLQUAT;
      psen->objtype = mjOBJ_JOINT;
      ReadAttrTxt(elem, "joint", objname, true);
    } else if (type=="ballangvel") {
      psen->type = mjSENS_BALLANGVEL;
      psen->objtype = mjOBJ_JOINT;
      ReadAttrTxt(elem, "joint", objname, true);
    }

    // joint and tendon limit sensors
    else if (type=="jointlimitpos") {
      psen->type = mjSENS_JOINTLIMITPOS;
      psen->objtype = mjOBJ_JOINT;
      ReadAttrTxt(elem, "joint", objname, true);
    } else if (type=="jointlimitvel") {
      psen->type = mjSENS_JOINTLIMITVEL;
      psen->objtype = mjOBJ_JOINT;
      ReadAttrTxt(elem, "joint", objname, true);
    } else if (type=="jointlimitfrc") {
      psen->type = mjSENS_JOINTLIMITFRC;
      psen->objtype = mjOBJ_JOINT;
      ReadAttrTxt(elem, "joint", objname, true);
    } else if (type=="tendonlimitpos") {
      psen->type = mjSENS_TENDONLIMITPOS;
      psen->objtype = mjOBJ_TENDON;
      ReadAttrTxt(elem, "tendon", objname, true);
    } else if (type=="tendonlimitvel") {
      psen->type = mjSENS_TENDONLIMITVEL;
      psen->objtype = mjOBJ_TENDON;
      ReadAttrTxt(elem, "tendon", objname, true);
    } else if (type=="tendonlimitfrc") {
      psen->type = mjSENS_TENDONLIMITFRC;
      psen->objtype = mjOBJ_TENDON;
      ReadAttrTxt(elem, "tendon", objname, true);
    }

    // sensors attached to an object with spatial frame: (x)body, geom, site, camera
    else if (type=="framepos") {
      psen->type = mjSENS_FRAMEPOS;
      ReadAttrTxt(elem, "objtype", text, true);
      psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      ReadAttrTxt(elem, "objname", objname, true);
      if (ReadAttrTxt(elem, "reftype", text)) {
        psen->reftype = (mjtObj)mju_str2Type(text.c_str());
        ReadAttrTxt(elem, "refname", refname, true);
      } else if (ReadAttrTxt(elem, "refname", text)) {
        throw mjXError(elem, "refname '%s' given but reftype is missing", text.c_str());
      }
    } else if (type=="framequat") {
      psen->type = mjSENS_FRAMEQUAT;
      ReadAttrTxt(elem, "objtype", text, true);
      psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      ReadAttrTxt(elem, "objname", objname, true);
      if (ReadAttrTxt(elem, "reftype", text)) {
        psen->reftype = (mjtObj)mju_str2Type(text.c_str());
        ReadAttrTxt(elem, "refname", refname, true);
      } else if (ReadAttrTxt(elem, "refname", text)) {
        throw mjXError(elem, "refname '%s' given but reftype is missing", text.c_str());
      }
    } else if (type=="framexaxis") {
      psen->type = mjSENS_FRAMEXAXIS;
      ReadAttrTxt(elem, "objtype", text, true);
      psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      ReadAttrTxt(elem, "objname", objname, true);
      if (ReadAttrTxt(elem, "reftype", text)) {
        psen->reftype = (mjtObj)mju_str2Type(text.c_str());
        ReadAttrTxt(elem, "refname", refname, true);
      } else if (ReadAttrTxt(elem, "refname", text)) {
        throw mjXError(elem, "refname '%s' given but reftype is missing", text.c_str());
      }
    } else if (type=="frameyaxis") {
      psen->type = mjSENS_FRAMEYAXIS;
      ReadAttrTxt(elem, "objtype", text, true);
      psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      ReadAttrTxt(elem, "objname", objname, true);
      if (ReadAttrTxt(elem, "reftype", text)) {
        psen->reftype = (mjtObj)mju_str2Type(text.c_str());
        ReadAttrTxt(elem, "refname", refname, true);
      } else if (ReadAttrTxt(elem, "refname", text)) {
        throw mjXError(elem, "refname '%s' given but reftype is missing", text.c_str());
      }
    } else if (type=="framezaxis") {
      psen->type = mjSENS_FRAMEZAXIS;
      ReadAttrTxt(elem, "objtype", text, true);
      psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      ReadAttrTxt(elem, "objname", objname, true);
      if (ReadAttrTxt(elem, "reftype", text)) {
        psen->reftype = (mjtObj)mju_str2Type(text.c_str());
        ReadAttrTxt(elem, "refname", refname, true);
      } else if (ReadAttrTxt(elem, "refname", text)) {
        throw mjXError(elem, "refname '%s' given but reftype is missing", text.c_str());
      }
    } else if (type=="framelinvel") {
      psen->type = mjSENS_FRAMELINVEL;
      ReadAttrTxt(elem, "objtype", text, true);
      psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      ReadAttrTxt(elem, "objname", objname, true);
      if (ReadAttrTxt(elem, "reftype", text)) {
        psen->reftype = (mjtObj)mju_str2Type(text.c_str());
        ReadAttrTxt(elem, "refname", refname, true);
      } else if (ReadAttrTxt(elem, "refname", text)) {
        throw mjXError(elem, "refname '%s' given but reftype is missing", text.c_str());
      }
    } else if (type=="frameangvel") {
      psen->type = mjSENS_FRAMEANGVEL;
      ReadAttrTxt(elem, "objtype", text, true);
      psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      ReadAttrTxt(elem, "objname", objname, true);
      if (ReadAttrTxt(elem, "reftype", text)) {
        psen->reftype = (mjtObj)mju_str2Type(text.c_str());
        ReadAttrTxt(elem, "refname", refname, true);
      } else if (ReadAttrTxt(elem, "refname", text)) {
        throw mjXError(elem, "refname '%s' given but reftype is missing", text.c_str());
      }
    } else if (type=="framelinacc") {
      psen->type = mjSENS_FRAMELINACC;
      ReadAttrTxt(elem, "objtype", text, true);
      psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      ReadAttrTxt(elem, "objname", objname, true);
    } else if (type=="frameangacc") {
      psen->type = mjSENS_FRAMEANGACC;
      ReadAttrTxt(elem, "objtype", text, true);
      psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      ReadAttrTxt(elem, "objname", objname, true);
    }

    // sensors related to kinematic subtrees; attached to a body (which is the subtree root)
    else if (type=="subtreecom") {
      psen->type = mjSENS_SUBTREECOM;
      psen->objtype = mjOBJ_BODY;
      ReadAttrTxt(elem, "body", objname, true);
    } else if (type=="subtreelinvel") {
      psen->type = mjSENS_SUBTREELINVEL;
      psen->objtype = mjOBJ_BODY;
      ReadAttrTxt(elem, "body", objname, true);
    } else if (type=="subtreeangmom") {
      psen->type = mjSENS_SUBTREEANGMOM;
      psen->objtype = mjOBJ_BODY;
      ReadAttrTxt(elem, "body", objname, true);
    }

    // global sensors
    else if (type=="clock") {
      psen->type = mjSENS_CLOCK;
      psen->objtype = mjOBJ_UNKNOWN;
    }

    // user-defined sensor
    else if (type=="user") {
      psen->type = mjSENS_USER;
      bool objname_given = ReadAttrTxt(elem, "objname", objname);
      if (ReadAttrTxt(elem, "objtype", text)) {
        if (!objname_given) {
          throw mjXError(elem, "objtype '%s' given but objname is missing", text.c_str());
        }
        psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      } else if (objname_given) {
        throw mjXError(elem, "objname '%s' given but objtype is missing", objname.c_str());
      }
      ReadAttrInt(elem, "dim", &psen->dim, true);

      // keywords
      if (MapValue(elem, "needstage", &n, stage_map, stage_sz)) {
        psen->needstage = (mjtStage)n;
      }
      if (MapValue(elem, "datatype", &n, datatype_map, datatype_sz)) {
       psen->datatype = (mjtDataType)n;
      }
    }

    else if (type=="plugin") {
      psen->type = mjSENS_PLUGIN;
      OnePlugin(elem, &psen->plugin);
      ReadAttrTxt(elem, "objtype", text);
      psen->objtype = (mjtObj)mju_str2Type(text.c_str());
      ReadAttrTxt(elem, "objname", objname);
      if (psen->objtype != mjOBJ_UNKNOWN && objname.empty()) {
        throw mjXError(elem, "objtype is specified but objname is not");
      }
      if (psen->objtype == mjOBJ_UNKNOWN && !objname.empty()) {
        throw mjXError(elem, "objname is specified but objtype is not");
      }
      if (ReadAttrTxt(elem, "reftype", text)) {
        psen->reftype = (mjtObj)mju_str2Type(text.c_str());
      }
      ReadAttrTxt(elem, "refname", refname);
      if (psen->reftype != mjOBJ_UNKNOWN && refname.empty()) {
        throw mjXError(elem, "reftype is specified but refname is not");
      }
      if (psen->reftype == mjOBJ_UNKNOWN && !refname.empty()) {
        throw mjXError(elem, "refname is specified but reftype is not");
      }
    }

    if (!objname.empty()) {
      mjm_setString(psen->objname, objname.c_str());
    }

    if (!refname.empty()) {
      mjm_setString(psen->refname, refname.c_str());
    }

    // write info
    mjm_setString(psen->info,
        std::string("line = " + std::to_string(elem->GetLineNum()) + ", column = -1").c_str());

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// keyframe section parser
void mjXReader::Keyframe(XMLElement* section) {
  XMLElement* elem;
  int n;
  double data[1000];

  // iterate over child elements
  elem = FirstChildElement(section);
  while (elem) {
    string text, name = "";

    // add keyframe
    mjmKey* pk = mjm_addKey(model);

    // read name, time
    ReadAttrTxt(elem, "name", name);
    mjm_setString(pk->name, name.c_str());
    ReadAttr(elem, "time", 1, &pk->time, text);

    // read qpos
    n = ReadAttr(elem, "qpos", 1000, data, text, false, false);
    if (n) {
      mjm_setDouble(pk->qpos, data, n);
    }

    // read qvel
    n = ReadAttr(elem, "qvel", 1000, data, text, false, false);
    if (n) {
      mjm_setDouble(pk->qvel, data, n);
    }

    // read act
    n = ReadAttr(elem, "act", 1000, data, text, false, false);
    if (n) {
      mjm_setDouble(pk->act, data, n);
    }

    // read mpos
    n = ReadAttr(elem, "mpos", 1000, data, text, false, false);
    if (n) {
      mjm_setDouble(pk->mpos, data, n);
    }

    // read mquat
    n = ReadAttr(elem, "mquat", 1000, data, text, false, false);
    if (n) {
      mjm_setDouble(pk->mquat, data, n);
    }

    // read ctrl
    n = ReadAttr(elem, "ctrl", 1000, data, text, false, false);
    if (n) {
      mjm_setDouble(pk->ctrl, data, n);
    }

    // advance to next element
    elem = NextSiblingElement(elem);
  }
}



// get defaults class
mjCDef* mjXReader::GetClass(XMLElement* section) {
  string text;
  mjCDef* def = 0;

  if (ReadAttrTxt(section, "class", text)) {
    def = model->FindDef(text);
    if (!def) {
      throw mjXError(section, "unknown default class");
    }
  }

  return def;
}



// get xml position
void mjXReader::GetXMLPos(XMLElement* elem, mjCBase* obj) {
  obj->info = "line = " + std::to_string(elem->GetLineNum());
}
