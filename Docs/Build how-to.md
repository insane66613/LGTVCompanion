# BUILD INSTRUCTIONS AND DEPENDENCIES
To build the current projects (UI, Service, Daemon, Console and Installer) use Visual Studio 2026 with the v145 C++ toolset and ensure that [Vcpkg](https://github.com/microsoft/vcpkg) is installed. Vcpkg is a free library manager for Windows. Visual Studio 2022/v143 can be used only by explicitly retargeting the projects and is not the repository default. You should use Vcpkg in one of two ways:

### Manifest
A Vcpkg manifest is included with the source code and the necessary dependencies will be automatically downloaded, configured and installed, if you choose to enable it. To enable the manifest please open the properties for each project in the solution, then 
navigate to the vcpkg section and Select "Yes" for "Use Manifest". Do so for the "Debug" and "Release" project configurations and X64 and ARM64 Platforms.

### Manual
Alternatively, You can manually install the dependencies, with the following commands:

		vcpkg install nlohmann-json:x64-windows-static
		vcpkg install boost-asio:x64-windows-static
		vcpkg install boost-beast:x64-windows-static
		vcpkg install wintoast:x64-windows-static
		vcpkg install openssl:x64-windows-static
  		vcpkg install phnt:x64-windows-static
    
  		vcpkg install nlohmann-json:arm64-windows-static
		vcpkg install boost-asio:arm64-windows-static
		vcpkg install boost-beast:arm64-windows-static
		vcpkg install wintoast:arm64-windows-static
		vcpkg install openssl:arm64-windows-static
  		vcpkg install phnt:arm64-windows-static

### Building the setup package
To build the setup package please ensure that a [HeatWave](https://www.firegiant.com/docs/heatwave/) extension compatible with your installed Visual Studio version is installed.
