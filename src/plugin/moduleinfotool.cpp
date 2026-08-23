// Nedit -- build tool. Generates Contents/Resources/moduleinfo.json for
// the assembled .vst3 bundle by loading the real module and interrogating
// its factory (exactly what the SDK's moduleinfotool sample does; the
// sample itself is not present in a shallow SDK checkout).
//
// Some hosts (e.g. Bitwig's indexer) refuse bundles without this file:
// "could not read metadata: Not a plug-in file".

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/moduleinfo/moduleinfocreator.h"

#include <fstream>
#include <iostream>

int main (int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: nedit_moduleinfotool <bundle.vst3> <out.json>\n";
        return 2;
    }

    std::string error;
    const auto module = VST3::Hosting::Module::create (argv[1], error);
    if (! module)
    {
        std::cerr << "module load failed: " << error << "\n";
        return 1;
    }

    const auto info = Steinberg::ModuleInfoLib::createModuleInfo (*module, false);

    std::ofstream out (argv[2]);
    if (! out)
    {
        std::cerr << "cannot open output: " << argv[2] << "\n";
        return 1;
    }

    Steinberg::ModuleInfoLib::outputJson (info, out);
    return 0;
}
