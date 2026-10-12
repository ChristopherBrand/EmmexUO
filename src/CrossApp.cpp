// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2020 Danny Angelo Carminati Grein

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string>
#include <xuocore/plugininterface.h>
#if defined(__APPLE__)
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#endif

// Resolve the client next to this executable: inside a .app bundle launched from
// Finder the working directory is '/', so the relative CROSSUO_CLIENT path fails.
static std::string client_path()
{
#if defined(__APPLE__)
    char exe[PATH_MAX];
    uint32_t len = sizeof(exe);
    char real[PATH_MAX];
    if (_NSGetExecutablePath(exe, &len) == 0 && realpath(exe, real))
        return std::string(dirname(real)) + "/" + (CROSSUO_CLIENT + 2); // skip "./"
#endif
    return CROSSUO_CLIENT;
}

int main(int argc, char **argv)
{
    typedef int (*plugin_main)(int, char **);

    const auto path = client_path();
    void *so = dlopen(path.c_str(), RTLD_LAZY);
    if (!so)
    {
        fprintf(stderr, "Could not load shared object '%s'\n", path.c_str());
        fprintf(stderr, "Reason: dlopen failed with: %s\n", dlerror());
        return -1;
    }

    plugin_main run = (plugin_main)dlsym(so, "plugin_main");
    if (!run)
    {
        dlclose(so);
        fprintf(stderr, "'%s' is invalid or corrupted\n", CROSSUO_CLIENT);
    }

    int r = run(argc, argv);
    dlclose(so);
    exit(r);
}
