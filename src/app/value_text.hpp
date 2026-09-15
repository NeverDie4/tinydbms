#ifndef TINYDBMS_APP_VALUE_TEXT_HPP
#define TINYDBMS_APP_VALUE_TEXT_HPP

#include <string>

#include "tinydbms/common.hpp"

namespace tinydbms::app {

// 展示层共用的值文本化：table 与 pretty 两种格式使用同一份映射，避免两处漂移。
// NULL 渲染为 NULL，BOOLEAN 渲染为 TRUE/FALSE，DOUBLE 至少保留一位小数。
std::string value_text(const tinydbms::Value& value);

// DOUBLE 的文本形式：std::to_chars 的最短往返表示；没有小数点的补 ".0"。
// to_chars 失败时返回占位文本，不抛异常、不产生控制字符。
std::string double_text(double value);

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_VALUE_TEXT_HPP
