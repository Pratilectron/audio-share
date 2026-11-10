/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0
*/

#ifndef LOCAL_DASHBOARD_HPP
#define LOCAL_DASHBOARD_HPP

#include <string>

// A page on http://127.0.0.1:65531/ for this PC only.
namespace local_dashboard {

void start();
void stop();
void note(const std::string& name, int clients);

} // namespace local_dashboard

#endif
