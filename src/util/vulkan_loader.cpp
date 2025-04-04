// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// No better place for this..
#define VMA_IMPLEMENTATION

#include "vulkan_loader.h"

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"

#ifdef ENABLE_SDL
#include <SDL3/SDL_vulkan.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

LOG_CHANNEL(GPUDevice);

extern "C" {

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name name;
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

void Vulkan::ResetVulkanLibraryFunctionPointers()
{
#define VULKAN_MODULE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) name = nullptr;
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

static DynamicLibrary s_vulkan_library;

#ifdef ENABLE_SDL
static bool s_vulkan_library_loaded_from_sdl = false;
#endif

bool Vulkan::IsVulkanLibraryLoaded()
{
#ifdef ENABLE_SDL
  return (s_vulkan_library.IsOpen() || s_vulkan_library_loaded_from_sdl);
#else
  return s_vulkan_library.IsOpen();
#endif
}

bool Vulkan::LoadVulkanLibrary(Error* error)
{
  AssertMsg(!s_vulkan_library.IsOpen(), "Vulkan module is not loaded.");

#ifdef __APPLE__
  // Check if a path to a specific Vulkan library has been specified.
  char* libvulkan_env = getenv("LIBVULKAN_PATH");
  if (libvulkan_env)
    s_vulkan_library.Open(libvulkan_env, error);
  if (!s_vulkan_library.IsOpen() &&
      !s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("MoltenVK").c_str(), error))
  {
    return false;
  }
#else
  // try versioned first, then unversioned.
  if (!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("vulkan", 1).c_str(), error) &&
      !s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("vulkan").c_str(), error))
  {
    return false;
  }
#endif

  bool required_functions_missing = false;

#define VULKAN_MODULE_ENTRY_POINT(name, required)                                                                      \
  if (!s_vulkan_library.GetSymbol(#name, &name) && required)                                                           \
  {                                                                                                                    \
    ERROR_LOG("Vulkan: Failed to load required module function {}", #name);                                            \
    required_functions_missing = true;                                                                                 \
  }
#include "vulkan_entry_points.inl"
#undef VULKAN_MODULE_ENTRY_POINT

  if (required_functions_missing)
  {
    Error::SetStringView(error, "One or more required functions are missing. The log contains more information.");
    ResetVulkanLibraryFunctionPointers();
    s_vulkan_library.Close();
    return false;
  }

  return true;
}

#ifdef ENABLE_SDL

bool Vulkan::LoadVulkanLibraryFromSDL(Error* error)
{
  if (!SDL_Vulkan_LoadLibrary(nullptr))
  {
    Error::SetStringFmt(error, "SDL_Vulkan_LoadLibrary() failed: {}", SDL_GetError());
    return false;
  }

  vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  if (!vkGetInstanceProcAddr)
  {
    Error::SetStringFmt(error, "SDL_Vulkan_GetVkGetInstanceProcAddr() failed: {}", SDL_GetError());
    SDL_Vulkan_UnloadLibrary();
    return false;
  }

  bool required_functions_missing = false;

  // vkGetInstanceProcAddr() can't resolve itself until Vulkan 1.2.

#define VULKAN_MODULE_ENTRY_POINT(name, required)                                                                      \
  if ((reinterpret_cast<const void*>(&name) != reinterpret_cast<const void*>(&vkGetInstanceProcAddr)) &&               \
      !(name = reinterpret_cast<decltype(name)>(vkGetInstanceProcAddr(nullptr, #name))) && required)                   \
  {                                                                                                                    \
    ERROR_LOG("Vulkan: Failed to load required module function {}", #name);                                            \
    required_functions_missing = true;                                                                                 \
  }
#include "vulkan_entry_points.inl"
#undef VULKAN_MODULE_ENTRY_POINT

  if (required_functions_missing)
  {
    Error::SetStringView(error, "One or more required functions are missing. The log contains more information.");
    ResetVulkanLibraryFunctionPointers();
    SDL_Vulkan_UnloadLibrary();
    return false;
  }

  s_vulkan_library_loaded_from_sdl = true;
  return true;
}

#endif

void Vulkan::UnloadVulkanLibrary()
{
  ResetVulkanLibraryFunctionPointers();

  s_vulkan_library.Close();

#ifdef ENABLE_SDL
  if (s_vulkan_library_loaded_from_sdl)
  {
    s_vulkan_library_loaded_from_sdl = false;
    SDL_Vulkan_UnloadLibrary();
  }
#endif
}

bool Vulkan::LoadVulkanInstanceFunctions(VkInstance instance)
{
  bool required_functions_missing = false;
  auto LoadFunction = [&](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
    *func_ptr = vkGetInstanceProcAddr(instance, name);
    if (!(*func_ptr) && is_required)
    {
      ERROR_LOG("Vulkan: Failed to load required instance function {}", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_INSTANCE_ENTRY_POINT(name, required)                                                                    \
  LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "vulkan_entry_points.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT

  return !required_functions_missing;
}

bool Vulkan::LoadVulkanDeviceFunctions(VkDevice device)
{
  bool required_functions_missing = false;
  auto LoadFunction = [&](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
    *func_ptr = vkGetDeviceProcAddr(device, name);
    if (!(*func_ptr) && is_required)
    {
      ERROR_LOG("Vulkan: Failed to load required device function {}", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_DEVICE_ENTRY_POINT(name, required)                                                                      \
  LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT

  return !required_functions_missing;
}
