#include "ShaderCompiler.h"

#include <windows.h> // dxcapi.h 依赖 COM/基础类型
#include <wrl/client.h>
#include <dxc/dxcapi.h>

#include <filesystem>
#include <fstream>
#include <format>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {

[[noreturn]] void throwCompileError(const char* what) {
	throw std::runtime_error(std::string(what));
}

// 把 IDxcBlob 转为字节向量。
std::vector<std::uint8_t> blobToBytes(ComPtr<IDxcBlob> blob) {
	std::vector<std::uint8_t> bytes(
		static_cast<const std::uint8_t*>(blob->GetBufferPointer()),
		static_cast<const std::uint8_t*>(blob->GetBufferPointer()) + blob->GetBufferSize());
	return bytes;
}

} // namespace

namespace shader {

std::vector<std::uint8_t> compileFromFile(const std::string& filePath, const CompileOptions& options) {
	std::ifstream file(filePath, std::ios::in | std::ios::binary);
	if (!file.is_open()) {
		throw std::runtime_error("Failed to open shader file: " + filePath);
	}
	std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return compileFromSource(source, filePath, options);
}

std::vector<std::uint8_t> compileFromSource(const std::string& source, const std::string& sourceName,
	const CompileOptions& options) {
	ComPtr<IDxcUtils> pUtils;
	ComPtr<IDxcCompiler3> pCompiler;
	ComPtr<IDxcIncludeHandler> pIncludeHandler;

	HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&pUtils));
	if (FAILED(hr)) {
		throwCompileError("Failed to create DxcUtils (dxcompiler.dll missing?)");
	}
	hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&pCompiler));
	if (FAILED(hr)) {
		throwCompileError("Failed to create DxcCompiler");
	}
	hr = pUtils->CreateDefaultIncludeHandler(&pIncludeHandler);
	if (FAILED(hr)) {
		throwCompileError("Failed to create include handler");
	}

	// 编码源码 blob（UTF-8）。
	ComPtr<IDxcBlobEncoding> pSource;
	hr = pUtils->CreateBlobFromPinned(source.data(), static_cast<UINT32>(source.size()),
		CP_UTF8, &pSource);
	if (FAILED(hr)) {
		throwCompileError("Failed to create source blob");
	}

	DxcBuffer sourceBuffer{};
	sourceBuffer.Ptr = pSource->GetBufferPointer();
	sourceBuffer.Size = pSource->GetBufferSize();
	sourceBuffer.Encoding = 0; // UTF-8

	// 转 UTF-16 的源码名，用于错误信息。
	std::wstring sourceNameW(sourceName.begin(), sourceName.end());

	std::vector<LPCWSTR> args;
	args.push_back(L"-E");
	args.push_back(options.entryPoint.c_str());
	args.push_back(L"-T");
	args.push_back(options.targetProfile.c_str());
	if (options.enableDebugInfo) {
		args.push_back(L"-Zi");
		args.push_back(L"-Qembed_debug");
	}
	if (options.skipOptimization) {
		args.push_back(L"-Od");
	} else {
		args.push_back(L"-O3");
	}
	args.push_back(sourceNameW.c_str());

	ComPtr<IDxcResult> pResult;
	hr = pCompiler->Compile(&sourceBuffer, args.data(), static_cast<UINT32>(args.size()),
		pIncludeHandler.Get(), IID_PPV_ARGS(&pResult));
	if (FAILED(hr)) {
		throwCompileError("dxc Compile call failed");
	}

	// 检查编译状态。
	HRESULT status = S_OK;
	pResult->GetStatus(&status);
	if (FAILED(status)) {
		ComPtr<IDxcBlobEncoding> pErrors;
		if (SUCCEEDED(pResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr)) && pErrors) {
			const char* msg = static_cast<const char*>(pErrors->GetBufferPointer());
			throw std::runtime_error(std::format("Shader compilation failed for {}:\n{}",
				sourceName, std::string(msg, pErrors->GetBufferSize())));
		}
		throwCompileError("Shader compilation failed (no error message)");
	}

	// 取对象字节码。
	ComPtr<IDxcBlob> pObject;
	hr = pResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pObject), nullptr);
	if (FAILED(hr) || !pObject) {
		throwCompileError("Failed to get shader object output");
	}
	return blobToBytes(pObject);
}

} // namespace shader
