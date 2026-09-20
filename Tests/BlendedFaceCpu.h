// SPDX-License-Identifier: MIT
#pragma once
#include "CpuShader.h"
namespace SilPOM::Cpu {
#define SP_REF(T) T&
#define SP_TEX const Texture&
#define SP_ZERO(T) T{}
#include "../Assets/Shaders/SilPOM/PlanarFace.azsli"
#include "../Assets/Shaders/SilPOM/BlendedFace.azsli"
#include "../Assets/Shaders/SilPOM/ReliefShadow.azsli"
#undef SP_REF
#undef SP_TEX
#undef SP_ZERO
}
