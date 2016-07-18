file(REMOVE_RECURSE
  "liblbann.pdb"
  "liblbann.dylib"
)

# Per-language clean rules from dependency scanning.
foreach(lang CXX)
  include(CMakeFiles/lbann.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
