set main_tag "2022.09"
set scripts_tag "e425576fbc481bd6528d3e7bfea3e2bf5800e199"; # added NVC support

exec rm -rf ../lib/*; # remove VHDL lib source from NVC repo (issue #557}
exec git clone --branch $main_tag --recurse-submodules -j8 https://github.com/OSVVM/OsvvmLibraries.git >@ stdout 2>@ stdout
cd OsvvmLibraries/Scripts
exec git checkout $scripts_tag >@ stdout 2>@ stdout
cd ../..
source OsvvmLibraries/Scripts/StartNVC.tcl
build OsvvmLibraries/OsvvmLibraries
build OsvvmLibraries/RunAllTests
build OsvvmLibraries/RunAllTestsVti
