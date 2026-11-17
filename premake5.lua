--[[
	This premake file is also loaded by our development/module/premake5.lua
	If this happens the DEVELOPMENT will be set to true
	I did this to have 1 premake file instead of 2 separate ones.
]]
PROJECT_GENERATOR_VERSION = 3

EXCLUDE_COMMON_PROJECT = true
if not DEVELOPMENT then
	newoption({
		trigger = "gmcommon",
		description = "Sets the path to the garrysmod_common (https://github.com/danielga/garrysmod_common) directory",
		default = "../garrysmod_common"
	})

	newoption({
		trigger = "dedicated",
		description = "Build for Windows dedicated server (defines DEDICATED)"
	})

	DEDICATED = _OPTIONS["dedicated"] and true or false
end

local gmcommon = assert(_OPTIONS.gmcommon or os.getenv("GARRYSMOD_COMMON"),
	"you didn't provide a path to your garrysmod_common (https://github.com/danielga/garrysmod_common) directory")
include(gmcommon)

local rootDir = ""
local sourcePath = "source/"
if DEVELOPMENT then
	rootDir = "../../"
	sourcePath = rootDir .. sourcePath
end
include(rootDir .. "overrides.lua")

include(sourcePath .. "bootil/premake5.lua")

--[[
	Project setup
	We MUST use abi as else many GMod structs do not match!
]]
-- The server build gets its own folder so it doesn't overwrite the client solution
local workspacePath = nil
if DEDICATED then
	workspacePath = (_OPTIONS["workspace"] or WORKSPACE_DIRECTORY) .. "_dedicated"
end

CreateWorkspace({name = "filesystem_stdio", abi_compatible = true, path = workspacePath})
	-- Serverside module (gmsv prefix)
	-- Can define "source_path", where the source files are located
	-- Can define "manual_files", which allows you to manually add files to the project,
	-- instead of automatically including them from the "source_path"
	-- Can also define "abi_compatible", for project specific compatibility
	CreateProject({serverside = true, manual_files = false, source_path = sourcePath:sub(0, -2)})
		kind("SharedLib")
		symbols("On")
		targetsuffix("")

		-- Remove some or all of these includes if they're not needed
		--IncludeHelpersExtended()
		--IncludeLuaShared()
		IncludeSDKCommon()
		IncludeSDKTier0()
		IncludeSDKTier1()
		IncludeSDKTier2()
		--IncludeSDKTier3()
		--IncludeSDKMathlib()
		--IncludeSDKRaytrace()
		--IncludeSDKBitmap()
		--IncludeSDKVTF()
		IncludeSteamAPI()
		--IncludeDetouring()
		--IncludeScanning()
		IncludeBootil()

		if DEDICATED then
			defines("SWDS=1")
		end

		defines("GMOD")
		defines("NO_VCR")

		-- Filesystem defs
		defines({"FILESYSTEM_STDIO_EXPORTS", "DONT_PROTECT_FILEIO_FUNCTIONS", "PROTECTED_THINGS_ENABLE"})
		-- GMod specific def
		defines("SUPPORT_PACKED_STORE")

		if GMOD_X86_64 then
			defines("GMOD_X86_64")
		end

		files({
			gmcommon .. [[/sourcesdk-minimal/public/tier0/memoverride.cpp]],
			gmcommon .. [[/sourcesdk-minimal/public/zip_utils.cpp]],
			gmcommon .. [[/sourcesdk-minimal/public/kevvaluescompiler.cpp]],
			rootDir .. "README.md",
			rootDir .. ".github/workflows/**.yml",
			sourcePath .. "garrysmod/**",
			sourcePath .. "vpklib/**",
		})

		vpaths({
			["Source files/sourcesdk/"] = gmcommon .. "/**.*",
			["README"] = rootDir .. "README.md",
			["Workflows"] = rootDir .. ".github/workflows/**.yml",
		})

		filter("system:windows")
			defines({"IVP_NO_MATH_INL", "COMPILER_MSVC"})
			disablewarnings({"4101"})
			files(sourcePath .. "tier0/platform.cpp")

		filter({"system:windows", "platforms:x86_64"})
			defines({"COMPILER_MSVC64", "WIN64"})

		filter({"system:linux", "platforms:x86_64"})
			buildoptions({"-mcx16"}) -- Should solve this: undefined reference to `__sync_bool_compare_and_swap_16'

		filter({"platforms:x86_64"})
			defines("PLATFORM_64BITS")

		filter("system:windows")
			removefiles(sourcePath .. "linux_support.cpp")
			if DEDICATED then
				defines("DEDICATED")
			else
				defines("NOT_DEDICATED") -- Windows client build
			end

		filter("system:linux")
			files(sourcePath .. "tier0/platform_posix.cpp")
			disablewarnings({"unused-variable"})
			targetextension(".so")
			links({"dl", "pthread"}) -- this fixes the undefined reference to `dlopen' errors. tier0 comes from IncludeSDKTier0
			defines({"DEDICATED", "POSIX"}) -- All linux build focus Linux dedicated servers.

	--[[
		Replacement for dedicated.dll / dedicated_srv.so.
		Stock has the whole filesystem compiled in, ours loads filesystem_stdio next to it.
		Windows only builds it with --dedicated, every Linux build is a server build.
	]]
	filter({})
	if DEDICATED or os.istarget("linux") then
		CreateProject({serverside = true, manual_files = true, source_path = rootDir .. "dedicated", name = "dedicated"})
			kind("SharedLib")
			symbols("On")
			targetsuffix("")
			targetname("dedicated")

			IncludeSDKCommon()
			IncludeSDKTier0()
			IncludeSDKTier1()
			IncludeSDKTier2()
			IncludeSteamAPI()

			filter({})
			defines({"GMOD", "NO_VCR", "DEDICATED", "SWDS=1"})
			if GMOD_X86_64 then
				defines("GMOD_X86_64")
			end

			includedirs(rootDir .. "dedicated")

			files({
				rootDir .. "dedicated/*.h",
				rootDir .. "dedicated/appframework.cpp",
				rootDir .. "dedicated/sys_common.cpp",
				rootDir .. "dedicated/sys_ded.cpp",
				rootDir .. "dedicated/console/textconsole.h",
				rootDir .. "dedicated/console/textconsole.cpp",
				gmcommon .. "/sourcesdk-minimal/public/filesystem_init.cpp",
				gmcommon .. "/sourcesdk-minimal/public/tier0/memoverride.cpp",
			})

			vpaths({["Source files/sourcesdk/"] = gmcommon .. "/**.*"})

			filter({"platforms:x86_64"})
				defines("PLATFORM_64BITS")

			filter("system:windows")
				defines({"COMPILER_MSVC"})
				files({
					rootDir .. "dedicated/sys_windows.cpp",
					rootDir .. "dedicated/console/conproc.*",
					rootDir .. "dedicated/console/TextConsoleWin32.*",
				})
				links({"user32", "shell32", "wsock32"})
				-- The minidump SE translator needs async exceptions
				buildoptions({"/EHa"})
				disablewarnings({"4101"})

			filter({"system:windows", "platforms:x86_64"})
				defines({"COMPILER_MSVC64", "WIN64"})

			filter("system:linux")
				defines({"POSIX"})
				files({
					rootDir .. "dedicated/sys_linux.cpp",
					rootDir .. "dedicated/console/TextConsoleUnix.*",
				})
				targetname("dedicated_srv")
				targetextension(".so")
				links({"dl", "pthread"})
				disablewarnings({"unused-variable"})

			filter({"system:linux", "platforms:x86_64"})
				buildoptions({"-mcx16"})

			filter({})
	end
