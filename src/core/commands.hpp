#pragma once
/*
 * Copyright (c) 2026 by Bert Laverman. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ostream>
#include <string_view>

#include "bus-core.hpp"


namespace nl::rakis::i2cbus {

/**
 * @brief Run one line of text commands on the core, and print the answer. The same commands are used by the command line tool
 * and, until there is an HTTP layer, by the service on its standard input.
 *
 *     set <display> <number>|blank      brightness <display> <0-15>
 *     online <board> | offline <board>  (for the tool; the service gets these from the bus)
 *     flush | show | events | help
 *
 * @return false for `quit`, true for everything else (also for a command that failed: the answer says so).
 */
bool runCommand(BusCore& core, std::string_view line, std::ostream& out);

} // namespace nl::rakis::i2cbus
