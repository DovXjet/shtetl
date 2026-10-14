#pragma once

#include "CompactMesh.h"

#include <QString>

#include <atomic>

// STEP and IGES files, read with OpenCASCADE (ported from XjetStudio's
// CADReader). Built only when OpenCASCADE is found (SHTETL_HAS_CAD).

// The meshing tolerances of the import, set in File > Preferences and kept in
// QSettings under the keys XjetStudio uses.
struct CadDeflection
{
    double linear = 0.01;  // mm: the largest distance of the mesh from the surface
    double angular = 0.5;  // rad: the largest angle between neighbouring facets
};

CadDeflection cadDeflectionFromSettings();
void saveCadDeflection(const CadDeflection& deflection);

// True for .step/.stp/.iges/.igs, whether or not this build can read them.
bool isCadExtension(const QString& lowerCaseSuffix);

// Reads all the solids (or else faces or shells) of the file into one mesh in
// millimeters. `progress` gets 0..100; *cancel turning true ends the read with
// std::runtime_error("Abort requested!"). Throws std::runtime_error.
IndexedMesh readCad(const QString& path, const CadDeflection& deflection, const std::atomic<bool>* cancel = nullptr,
                    const LoadProgress& progress = {});
