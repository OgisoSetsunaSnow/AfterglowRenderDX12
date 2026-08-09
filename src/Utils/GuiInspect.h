// 反射驱动函数按钮渲染（照搬参考项目 AfterglowPanel::renderReflectionContext + renderInput + dispatchInvocation）。
// 用法：类用 INR_CLASS/INR_FUNCS 宏注册成员函数（见参考 AfterglowRenderStatus.h），然后
//   guiInspect::renderFunctions<MyClass>(instance, logCallback);
// 自动为每个注册函数生成：参数输入控件（按类型）+ 调用按钮，点击后把返回值/信息写入日志。
#pragma once

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

// Inreflect.h 使用 std::max/std::min：防御 Windows 的 min/max 宏。
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#include "Inreflect.h"

namespace guiInspect {

// 参数类型标签（仿参考 AfterglowPanel::renderInputTypeHint 的命名）。
template<typename T>
inline const char* typeName() { return "?"; }
template<> inline const char* typeName<bool>() { return "bool"; }
template<> inline const char* typeName<int>() { return "int"; }
template<> inline const char* typeName<std::uint32_t>() { return "uint"; }
template<> inline const char* typeName<float>() { return "float"; }
template<> inline const char* typeName<double>() { return "double"; }
template<> inline const char* typeName<glm::vec3>() { return "float3"; }
template<> inline const char* typeName<glm::vec4>() { return "float4"; }
template<> inline const char* typeName<std::string>() { return "string"; }

// 枚举下拉（仿参考 AfterglowPanel::renderInput 的 enum 分支，用 Inreflect 枚举属性生成选项）。
template<typename T>
inline bool renderEnumInput(T& value) {
	static std::vector<std::string> itemStrs;
	static std::once_flag itemFlag;
	std::call_once(itemFlag, []() {
		itemStrs.clear();
		Inreflect<T>::forEachAttribute([](auto enumInfo) {
			itemStrs.emplace_back(enumInfo.name);
		});
	});
	const int current = static_cast<int>(value);
	if (ImGui::BeginCombo("##enum",
			current >= 0 && current < static_cast<int>(itemStrs.size()) ? itemStrs[current].c_str() : "?")) {
		for (int i = 0; i < static_cast<int>(itemStrs.size()); ++i) {
			const bool selected = (current == i);
			if (ImGui::Selectable(itemStrs[i].c_str(), selected)) {
				value = static_cast<T>(i);
			}
		}
		ImGui::EndCombo();
	}
	return true;
}

// 参数输入控件（仿参考 AfterglowPanel::renderInput 的类型重载，用 if constexpr 分发）。
template<typename T>
inline bool renderInput(T& value) {
	ImGui::PushID(typeName<T>());
	if constexpr (std::is_same_v<T, bool>) {
		ImGui::Checkbox("##value", &value);
		ImGui::PopID();
		return true;
	}
	else if constexpr (std::is_same_v<T, int>) {
		ImGui::InputInt("##value", &value);
		ImGui::PopID();
		return true;
	}
	else if constexpr (std::is_same_v<T, std::uint32_t>) {
		ImGui::InputScalar("##value", ImGuiDataType_U32, &value);
		ImGui::PopID();
		return true;
	}
	else if constexpr (std::is_same_v<T, float>) {
		ImGui::InputFloat("##value", &value, 0.0f, 0.0f, "%.4g");
		ImGui::PopID();
		return true;
	}
	else if constexpr (std::is_same_v<T, double>) {
		ImGui::InputDouble("##value", &value, 0.0, 0.0, "%.6g");
		ImGui::PopID();
		return true;
	}
	else if constexpr (std::is_same_v<T, glm::vec3>) {
		ImGui::InputFloat3("##value", glm::value_ptr(value), "%.3g");
		ImGui::PopID();
		return true;
	}
	else if constexpr (std::is_same_v<T, glm::vec4>) {
		ImGui::InputFloat4("##value", glm::value_ptr(value), "%.3g");
		ImGui::PopID();
		return true;
	}
	else if constexpr (std::is_enum_v<T>) {
		ImGui::PopID();
		return renderEnumInput(value);
	}
	else {
		// 不支持的类型：按钮禁用。
		ImGui::PopID();
		return false;
	}
}

// 返回值格式化（仿参考 dispatchInvocation 的消息格式化）。
template<typename T>
inline std::string formatValue(const T& v) {
	if constexpr (std::is_same_v<T, bool>) {
		return v ? "true" : "false";
	}
	else if constexpr (std::is_arithmetic_v<T>) {
		return std::to_string(v);
	}
	else if constexpr (std::is_same_v<T, std::string>) {
		return v;
	}
	else if constexpr (std::is_same_v<T, glm::vec3>) {
		return std::format("({:.3f}, {:.3f}, {:.3f})", v.x, v.y, v.z);
	}
	else if constexpr (std::is_same_v<T, glm::vec4>) {
		return std::format("({:.3f}, {:.3f}, {:.3f}, {:.3f})", v.x, v.y, v.z, v.w);
	}
	else {
		return "value";
	}
}

// 调用反射函数并把返回值转成日志文本（仿参考 dispatchInvocation）。
template<typename Class, typename FuncInfo, typename Tuple>
inline std::string invokeFunction(Class& instance, FuncInfo funcInfo, Tuple& params) {
	using Return = typename FuncInfo::Return;
	if constexpr (std::is_void_v<Return>) {
		FuncInfo::call(instance, params);
		return std::string();
	}
	else {
		Return result = FuncInfo::call(instance, params);
		return std::format("{} = {}", funcInfo.name.data(), formatValue(result));
	}
}

// 渲染类的所有反射函数：参数输入 + 调用按钮（仿参考 AfterglowPanel::renderReflectionContext）。
template<typename Class>
inline void renderFunctions(Class& instance, const std::function<void(const std::string&)>& log) {
	Inreflect<Class>::forEachFunction([&](auto funcInfo) {
		using FuncInfo = decltype(funcInfo);
		using ParamTuple = typename FuncInfo::ParamTuple;
		using PureTuple = inreflect::PureTuple<ParamTuple>;

		// 参数存储：每个函数（FuncInfo 实例化）一套独立 static。
		static std::unique_ptr<PureTuple> params = nullptr;
		if (!params) {
			params = std::make_unique<PureTuple>();
		}
		bool paramsValid = true;

		ImGui::PushID(funcInfo.name.data());
		ImGui::BeginChild("##fn", ImVec2(0, 0),
			ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize);
		ImGui::Text("%s", funcInfo.name.data());
		ImGui::Separator();
		if constexpr (std::tuple_size_v<ParamTuple> == 0) {
			paramsValid = true;
		}
		else {
			funcInfo.forEachParam([&]<typename ParamType, std::uint32_t ParamIndex>() {
				ImGui::PushID(static_cast<int>(ParamIndex) * 1009);
				paramsValid &= renderInput(std::get<ParamIndex>(*params));
				ImGui::PopID();
			});
		}
		if (paramsValid && ImGui::Button("Call", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
			const std::string result = invokeFunction(instance, funcInfo, *params);
			if (!result.empty()) {
				log(result);
			}
		}
		ImGui::EndChild();
		ImGui::PopID();
	});
}

} // namespace guiInspect
