// app/gui/embedded_fonts.cpp - pisma zabudovana primo do binarky.
//
// CMake pri buildu prozene kazde TTF pres ImGui nastroj binary_to_compressed_c
// a vysledna komprimovana C pole se prilinkuji sem. ithaca-gui tedy za behu
// nema zadnou zavislost na assetech — staci mu vlastni state.json.
//
// Generovane .inl deklaruji pole jako `static`, proto SMI byt includnuta
// v presne jednom translation unitu — jinak by se stovky KB duplikovaly
// do kazdeho, kdo includne theme.h.
//
// Licence: obe rodiny jsou pod SIL Open Font License, texty licenci lezi
// v third-party/fonts/. OFL zabudovani do software vyslovne dovoluje;
// text licence je nutne distribuovat s produktem.
#include "theme.h"

#include "JetBrainsMono.inl"          // rozhrani (jedina vaha)
#include "BarlowCondensedBold.inl"    // wordmark
#include "BarlowCondensedLight.inl"   // wordmark

namespace ithaca::gui::theme {

FontBlob monoBlob() {
    return { JetBrainsMono_compressed_data, JetBrainsMono_compressed_size };
}
FontBlob brandBoldBlob() {
    return { BarlowCondensedBold_compressed_data, BarlowCondensedBold_compressed_size };
}
FontBlob brandLightBlob() {
    return { BarlowCondensedLight_compressed_data, BarlowCondensedLight_compressed_size };
}

} // namespace ithaca::gui::theme
