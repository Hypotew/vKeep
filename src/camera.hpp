#pragma once

#include <string>

// Chemin de la première vraie webcam utilisable, ou chaîne vide s'il n'y en a pas.
// N'active pas la capture : le voyant de la caméra reste éteint.
std::string find_camera();
