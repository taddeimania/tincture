// FilterTypes.cpp — the filter type list (menu order) and categories.
// Filters.cpp implements each by index; keep the two in step.
#include "ParamIds.h"

namespace tincture::choices
{
const std::vector<std::string>& filterTypes()
{
    static const std::vector<std::string> l {
        // Normal (0..17)
        "MG Low 6", "MG Low 12", "MG Low 18", "MG Low 24", "Low 6", "Low 12", "Low 18", "Low 24",
        "High 6", "High 12", "High 18", "High 24", "Band 12", "Band 24", "Peak 12", "Peak 24", "Notch 12", "Notch 24",
        // Multi (18..37)
        "LH 12", "LB 12", "LP 12", "LN 12", "HB 12", "HP 12", "HN 12", "BP 12", "BN 12", "PP 12", "PN 12", "NN 12",
        "LBH 12", "LBH 24", "LPH 12", "LPH 24", "LNH 12", "LNH 24", "BPN 12", "BPN 24",
        // Flanges (38..46)
        "Cmb L", "Flg L", "Phs L", "Cmb H", "Flg H", "Phs H", "Cmb HL", "Flg HL", "Phs HL",
        // Misc (47..71)
        "Low EQ 6", "Low EQ 12", "Band EQ 12", "High EQ 6", "High EQ 12", "Ring Mod", "Ring Modx2", "SampHold", "SampHold-",
        "Combs", "Allpasses", "Reverb", "French LP", "German LP", "Add Bass", "Formant-I", "Formant-II", "Formant-III",
        "Bandreject", "Dist.Comb 1 LP", "Dist.Comb 1 BP", "Dist.Comb 2 LP", "Dist.Comb 2 BP", "Scream LP", "Scream BP",
        // New (72..83)
        "Wsp", "DJ Mixer", "Diffusor", "MG Ladder", "Acid Ladder", "EMS Ladder", "MG Dirty", "PZ SVF", "Comb 2", "Exp MM", "Exp BPF"
    };
    return l;
}

const std::vector<Category>& filterCategories()
{
    static const std::vector<Category> c { { "Normal", 0, 18 }, { "Multi", 18, 20 }, { "Flanges", 38, 9 }, { "Misc", 47, 25 }, { "New", 72, 11 } };
    return c;
}

// The VAR knob caption per type (spec C §1.5); "" = knob unused for this type.
const char* filterVarLabel (int type)
{
    if (type < 0) return "";
    if (type < 18) return "FAT";                        // Normal
    if (type < 30) return "FREQ";                       // Multi: dual
    if (type < 38) return "MORPH";                      // Multi: morphing
    if (type < 47)                                      // Flanges: L, H, HL groups of three
    {
        static const char* const fl[] { "LP FRQ", "HP FRQ", "HL WID" };
        return fl[(type - 38) / 3];
    }
    if (type < 52) return "DB +/-";                     // EQ types
    switch (type)
    {
        case 52: return "";                             // Ring Mod
        case 53: return "SPREAD";                       // Ring Modx2
        case 54: case 55: return "";                    // SampHold, SampHold-
        case 56: case 57: case 58: return "DAMP";       // Combs, Allpasses, Reverb
        case 59: return "BOEUF";                        // French LP
        case 60: return "";                             // German LP
        case 61: return "THRU";                         // Add Bass
        case 62: case 63: case 64: return "FORMNT";     // Formant-I/II/III
        case 65: return "WIDTH";                        // Bandreject
        case 66: case 67: case 68: case 69: return "COMBFRQ";
        case 70: case 71: return "SCREAM";
        case 72: return "MORPH";                        // Wsp
        case 73: return "";                             // DJ Mixer
        case 74: return "STAGES";                       // Diffusor
        case 75: case 76: case 77: return "SMOOTH";     // MG / Acid / EMS Ladder
        case 78: return "PAIN";                         // MG Dirty
        case 79: return "SMOOTH";                       // PZ SVF
        case 80: return "FRQ2";                         // Comb 2
        case 81: return "MIX";                          // Exp MM
        default: return "";                             // Exp BPF, out of range
    }
}
} // namespace tincture::choices
