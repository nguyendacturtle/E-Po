import os
import shutil
import re

# Path config
SOURCE_DIR = r"..\Rc_Engine_Sound_ESP32\src\vehicles\sounds"
DEST_DIR = r"src\sounds"
REGISTRY_FILE = r"src\sound_registry.h"

# Configure exactly which vehicles to load.
# You can uncomment or add more here, up to ~15 vehicles for a 3MB partition.
SELECTED_VEHICLES = [
    "DefenderV8OpenPipe",
    "ScaniaV8",
    "1965FordMustangV8",
    "JeepWranglerRubicon392V8",
    "Cat3406B",
    "VwBeetle",
    "HarleyDavidsonFXSB",
    "MercedesActros1836",
    "LaFerrari",
    "1000HpScaniaV8"
]

def main():
    print("Building sound registry...")
    
    if not os.path.exists(DEST_DIR):
        os.makedirs(DEST_DIR)
        
    all_files = os.listdir(SOURCE_DIR)
    
    vehicles = []
    
    for base in SELECTED_VEHICLES:
        idle_file = None
        rev_file = None
        start_file = None
        
        # Search case-insensitively for the matching files
        for f in all_files:
            fl = f.lower()
            if fl == f"{base.lower()}idle.h":
                idle_file = f
            elif fl == f"{base.lower()}rev.h":
                rev_file = f
            elif fl == f"{base.lower()}start.h":
                start_file = f
                
        if idle_file and rev_file:
            vehicles.append({
                "base": base,
                "idle": idle_file,
                "rev": rev_file,
                "start": start_file
            })
            # Copy to dest
            print(f"Copying {base}...")
            shutil.copy(os.path.join(SOURCE_DIR, idle_file), os.path.join(DEST_DIR, idle_file))
            shutil.copy(os.path.join(SOURCE_DIR, rev_file), os.path.join(DEST_DIR, rev_file))
            if start_file:
                shutil.copy(os.path.join(SOURCE_DIR, start_file), os.path.join(DEST_DIR, start_file))
        else:
            print(f"WARNING: Vehicle {base} missing Idle or Rev file. Skipping.")

    print(f"\nFound {len(vehicles)} valid vehicles out of {len(SELECTED_VEHICLES)} requested.")
    
    # Generate C++ code
    cpp_code = """// AUTO-GENERATED FILE. DO NOT EDIT.
#pragma once
#include <Arduino.h>

struct EngineSoundProfile {
    const char* name;
    
    const signed char* idleSamples;
    uint32_t idleSampleCount;
    uint32_t idleSampleRate;
    
    const signed char* revSamples;
    uint32_t revSampleCount;
    uint32_t revSampleRate;
    
    const signed char* startSamples;
    uint32_t startSampleCount;
    uint32_t startSampleRate;
    
    bool hasStart;
};

"""
    for idx, v in enumerate(vehicles):
        cpp_code += f"namespace Sound_{idx} {{\n"
        cpp_code += f"  #include \"sounds/{v['idle']}\"\n"
        cpp_code += f"  #include \"sounds/{v['rev']}\"\n"
        if v["start"]:
            cpp_code += f"  #include \"sounds/{v['start']}\"\n"
        cpp_code += "}\n\n"
        
    cpp_code += f"const int SOUND_PROFILE_COUNT = {len(vehicles)};\n"
    cpp_code += "const EngineSoundProfile soundProfiles[] = {\n"
    for idx, v in enumerate(vehicles):
        ns = f"Sound_{idx}"
        start_ptr = f"{ns}::startSamples" if v["start"] else "nullptr"
        start_cnt = f"{ns}::startSampleCount" if v["start"] else "0"
        start_rate = f"{ns}::startSampleRate" if v["start"] else "0"
        has_start = "true" if v["start"] else "false"
        
        cpp_code += f'    {{ "{v["base"]}",\n'
        cpp_code += f'      {ns}::samples, {ns}::sampleCount, {ns}::sampleRate,\n'
        cpp_code += f'      {ns}::revSamples, {ns}::revSampleCount, {ns}::revSampleRate,\n'
        cpp_code += f'      {start_ptr}, {start_cnt}, {start_rate}, {has_start} }},\n'
        
    cpp_code += "};\n"
    
    with open(REGISTRY_FILE, "w", encoding="utf-8") as f:
        f.write(cpp_code)
        
    print(f"Successfully generated {REGISTRY_FILE}.")

if __name__ == "__main__":
    main()
