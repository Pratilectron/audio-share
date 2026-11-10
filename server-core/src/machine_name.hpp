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

#ifndef MACHINE_NAME_HPP
#define MACHINE_NAME_HPP

#include <string>

// A stable name for this computer, derived from the OS machine id.
// Reinstalling the server or the phone app does not change it.
namespace machine_name {

// Empty when this computer has no machine id.
std::string local_name();

// Auth token both sides derive from the name the user types.
std::string token_for(const std::string& name);

// Directory topic for this name. Not shown to the user.
std::string topic_for(const std::string& name);

} // namespace machine_name

#endif
