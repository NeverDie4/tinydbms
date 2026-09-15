#ifndef TINYDBMS_APP_TERMINAL_HPP
#define TINYDBMS_APP_TERMINAL_HPP

namespace tinydbms::app {

bool stdin_is_terminal() noexcept;

// stdout 是否连到终端：决定未显式指定 --format 时的默认展示格式。
bool stdout_is_terminal() noexcept;

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_TERMINAL_HPP
