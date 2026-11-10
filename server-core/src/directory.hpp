/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef DIRECTORY_HPP
#define DIRECTORY_HPP

#include <string>

// Publishes this PC's current public endpoint under its stable name,
// so the phone can connect with the name instead of an address.
namespace directory {

bool publish(const std::string& name, const std::string& endpoint);

} // namespace directory

#endif
