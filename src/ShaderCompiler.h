#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 使用 dxcompiler.dll（IDxcCompiler3）把 HLSL 编译为 DXIL 字节码。
namespace shader {

struct CompileOptions {
	std::wstring entryPoint;
	std::wstring targetProfile; // 如 L"vs_6_0" / L"ps_6_0" / L"cs_6_0"
	bool enableDebugInfo = false;
	bool skipOptimization = false;
};

// 从文件中读取源码并编译。
std::vector<std::uint8_t> compileFromFile(const std::string& filePath, const CompileOptions& options);

// 从字符串源码编译。
std::vector<std::uint8_t> compileFromSource(const std::string& source, const std::string& sourceName,
	const CompileOptions& options);

} // namespace shader
