// app/gui/embedded_font.cpp - Cormorant zabudovany primo do binarky.
//
// Font se NEnacita ze souboru: CMake pri buildu prozene TTF pres ImGui nastroj
// binary_to_compressed_c a vysledne komprimovane C pole se prilinkuje sem.
// Diky tomu nema ithaca-gui za behu zadnou zavislost na assetech — staci mu
// vlastni state.json. (Drive se TTF hledal relativne k CWD, takze spusteni
// binarky z jineho adresare skoncilo u default ImGui fontu.)
//
// Generovany .inl deklaruje pole jako `static`, proto SMI byt includnuty
// v presne jednom translation unitu — jinak by se 290 KB dat duplikovalo
// do kazdeho, kdo by includnul theme.h.
//
// Licence: Cormorant je pod SIL Open Font License (third-party/cormorant/OFL.txt).
// OFL zabudovani do software vyslovne dovoluje; text licence je nutne
// distribuovat s produktem.
#include "theme.h"

#include "cormorant_medium.inl"   // generovano do ${CMAKE_BINARY_DIR}/generated

namespace ithaca::gui::theme {

const unsigned int* cormorantCompressedData() { return CormorantMedium_compressed_data; }
unsigned int        cormorantCompressedSize() { return CormorantMedium_compressed_size; }

} // namespace ithaca::gui::theme
