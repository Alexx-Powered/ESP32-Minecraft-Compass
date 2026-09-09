#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$test_dir/../.." && pwd)"
idf_dir="${1:-${IDF_PATH:?Pass ESP-IDF directory or set IDF_PATH}}"
output_dir="$project_dir/build-host-tests"
mkdir -p -- "$output_dir"
# Compile production code, not a copied parser. Sanitizers check all rejection
# paths and injected allocation failures without touching an ESP32 or real NVS.
"${CC:-cc}" -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I"$test_dir/stubs" -I"$project_dir/main" \
    -I"$idf_dir/components/json/cJSON" "$test_dir/test_geojson.c" \
    "$project_dir/main/geo_config.c" "$idf_dir/components/json/cJSON/cJSON.c" \
    -lm -o "$output_dir/test_geojson"
"$output_dir/test_geojson" "$project_dir/docs/examples/beihang-fence.geojson"
