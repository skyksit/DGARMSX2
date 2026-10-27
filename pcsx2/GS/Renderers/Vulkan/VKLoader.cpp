// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/Error.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <mutex>
#include <unistd.h>
#include "adrenotools/driver.h"
#endif

extern "C" {

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name name;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

void Vulkan::ResetVulkanLibraryFunctionPointers()
{
#define VULKAN_MODULE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) name = nullptr;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

static DynamicLibrary s_vulkan_library;

#if defined(__ANDROID__)
namespace
{
	std::mutex s_custom_driver_mutex;
	std::string s_custom_driver_dir;
	std::string s_custom_driver_name;
	std::string s_custom_redirect_dir;
	std::string s_custom_hook_lib_dir;
	Vulkan::CustomDriverStatus s_custom_driver_status;
} // namespace

void Vulkan::SetCustomDriverPath(const char* driver_dir, const char* driver_name,
	const char* redirect_dir, const char* hook_lib_dir)
{
	std::lock_guard lock(s_custom_driver_mutex);
	s_custom_driver_dir   = driver_dir   ? driver_dir   : "";
	s_custom_driver_name  = driver_name  ? driver_name  : "";
	s_custom_redirect_dir = redirect_dir ? redirect_dir : "";
	s_custom_hook_lib_dir = hook_lib_dir ? hook_lib_dir : "";
}

Vulkan::CustomDriverStatus Vulkan::GetCustomDriverStatus()
{
	std::lock_guard lock(s_custom_driver_mutex);
	return s_custom_driver_status;
}

void Vulkan::SetActiveDeviceDriverInfo(u32 driver_id, u32 api_version, const char* driver_name,
	const char* driver_info, const char* device_name)
{
	std::lock_guard lock(s_custom_driver_mutex);
	s_custom_driver_status.has_device = true;
	s_custom_driver_status.driver_id = driver_id;
	s_custom_driver_status.api_version = api_version;
	s_custom_driver_status.driver_name = driver_name ? driver_name : "";
	s_custom_driver_status.driver_info = driver_info ? driver_info : "";
	s_custom_driver_status.device_name = device_name ? device_name : "";
}

static void RecordCustomDriverFallback(std::string reason)
{
	std::lock_guard lock(s_custom_driver_mutex);
	s_custom_driver_status.loaded = false;
	s_custom_driver_status.fallback_reason = std::move(reason);
}

static bool TryOpenAdrenotoolsDriver(DynamicLibrary& library, Error* error)
{
	std::string driver_dir, driver_name, redirect_dir, hook_lib_dir;
	{
		std::lock_guard lock(s_custom_driver_mutex);
		s_custom_driver_status = {};
		if (s_custom_driver_dir.empty() || s_custom_driver_name.empty() || s_custom_hook_lib_dir.empty())
			return false;
		driver_dir   = s_custom_driver_dir;
		driver_name  = s_custom_driver_name;
		redirect_dir = s_custom_redirect_dir;
		hook_lib_dir = s_custom_hook_lib_dir;
		s_custom_driver_status.requested = true;
		s_custom_driver_status.library_name = driver_name;
	}

	// adrenotools dlopens these by name from hook_lib_dir; a host that forgot to ship them
	// otherwise only sees an opaque dlerror from inside the private linker namespace.
	for (const char* hook : {"libhook_impl.so", "libmain_hook.so"})
	{
		std::string hook_path = hook_lib_dir;
		if (!hook_path.empty() && hook_path.back() != '/')
			hook_path += '/';
		hook_path += hook;
		if (access(hook_path.c_str(), R_OK) != 0)
		{
			std::string reason = "missing adrenotools hook library " + hook_path;
			Console.Warning("VKLoader: %s — falling back to system loader.", reason.c_str());
			Error::SetString(error, reason);
			RecordCustomDriverFallback(std::move(reason));
			return false;
		}
	}

	int feature_flags = ADRENOTOOLS_DRIVER_CUSTOM;
	if (!redirect_dir.empty())
		feature_flags |= ADRENOTOOLS_DRIVER_FILE_REDIRECT;

	Console.WriteLn("VKLoader: opening custom Vulkan driver via adrenotools "
		"(dir=%s name=%s redirect=%s hook=%s)",
		driver_dir.c_str(), driver_name.c_str(),
		redirect_dir.empty() ? "<none>" : redirect_dir.c_str(),
		hook_lib_dir.c_str());

	// adrenotools_open_libvulkan takes tmpLibDir for API < 29 fallback; pass null to
	// use memfd which is fine on every modern device. The trailing slash in driver_dir /
	// redirect_dir is required by the driver's path resolution; the Kotlin side appends it.
	void* handle = adrenotools_open_libvulkan(
		RTLD_NOW, feature_flags,
		nullptr, // tmpLibDir (memfd path)
		hook_lib_dir.c_str(),
		driver_dir.c_str(),
		driver_name.c_str(),
		redirect_dir.empty() ? nullptr : redirect_dir.c_str(),
		nullptr // userMappingHandle (unused)
	);

	if (!handle)
	{
		const char* err = dlerror();
		Error::SetStringFmt(error,
			"adrenotools_open_libvulkan failed for {} ({}): {}",
			driver_name, driver_dir, err ? err : "<no dlerror>");
		Console.Warning("VKLoader: %s — falling back to system loader.", error ? error->GetDescription().c_str() : "custom driver load failed");
		RecordCustomDriverFallback(fmt::format("adrenotools_open_libvulkan failed for {}: {}",
			driver_name, err ? err : "<no dlerror>"));
		return false;
	}

	library.Adopt(handle);
	{
		std::lock_guard lock(s_custom_driver_mutex);
		s_custom_driver_status.loaded = true;
	}
	Console.WriteLn("VKLoader: adrenotools driver handle acquired.");
	return true;
}
#endif

bool Vulkan::IsVulkanLibraryLoaded()
{
	return s_vulkan_library.IsOpen();
}

bool Vulkan::LoadVulkanLibrary(Error* error)
{
	pxAssertRel(!s_vulkan_library.IsOpen(), "Vulkan module is not loaded.");

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
#if defined(__ANDROID__)
	// User-picked custom driver (e.g. Mesa Turnip from K11MCH1/AdrenoToolsDrivers)
	// takes priority. Falls back to the system loader on any failure so the boot
	// still proceeds — the Console.Warning above already logged the cause.
	if (TryOpenAdrenotoolsDriver(s_vulkan_library, error))
	{
		Error::Clear(error);
	}
	else
#endif
	// try versioned first, then unversioned.
	if (!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("vulkan", 1).c_str(), error) &&
		!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("vulkan").c_str(), error))
	{
		return false;
	}
#endif

	bool required_functions_missing = false;
#define VULKAN_MODULE_ENTRY_POINT(name, required) \
	if (!s_vulkan_library.GetSymbol(#name, &name)) \
	{ \
		ERROR_LOG("Vulkan: Failed to load required module function {}", #name); \
		required_functions_missing = true; \
	}

#include "VKEntryPoints.inl"
#undef VULKAN_MODULE_ENTRY_POINT

	if (required_functions_missing)
	{
		ResetVulkanLibraryFunctionPointers();
		s_vulkan_library.Close();
		return false;
	}

	return true;
}

void Vulkan::UnloadVulkanLibrary()
{
	ResetVulkanLibraryFunctionPointers();
	s_vulkan_library.Close();
}

bool Vulkan::LoadVulkanInstanceFunctions(VkInstance instance)
{
	bool required_functions_missing = false;
	auto LoadFunction = [&required_functions_missing, instance](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
		*func_ptr = vkGetInstanceProcAddr(instance, name);
		if (!(*func_ptr) && is_required)
		{
			std::fprintf(stderr, "Vulkan: Failed to load required instance function %s\n", name);
			required_functions_missing = true;
		}
	};

#define VULKAN_INSTANCE_ENTRY_POINT(name, required) \
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT

	return !required_functions_missing;
}

bool Vulkan::LoadVulkanDeviceFunctions(VkDevice device)
{
	bool required_functions_missing = false;
	auto LoadFunction = [&required_functions_missing, device](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
		*func_ptr = vkGetDeviceProcAddr(device, name);
		if (!(*func_ptr) && is_required)
		{
			std::fprintf(stderr, "Vulkan: Failed to load required device function %s\n", name);
			required_functions_missing = true;
		}
	};

#define VULKAN_DEVICE_ENTRY_POINT(name, required) \
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT

	return !required_functions_missing;
}
