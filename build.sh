    echo -e "Making marmite zImage\n"
    export PATH=$PATH:/opt/toolchain2/bin/
    export ARCH=arm
    export SUBARCH=arm
    export CROSS_COMPILE=arm-zen-linux-gnueabi-

# delete everything
#rm -fR kernelinjector.oxp/structure.new/modules/*
#rm -f kernelinjector.oxp/zImage.new/zImage

# cd kernelinjector.oxp
# sh extract
# cd ..

git apply -R cm_ioctrl.patch

# make
make -j3

# copy modules
find ./ -type f -name '*.ko' -exec cp -f {} ../marmite_zip/system/modules \;

# copy zImage
cp -f arch/arm/boot/zImage ../marmite_zip/kernel/

cd ../marmite_zip
zip -r ../marmite.zip * > /dev/null 2>&1
mv ../marmite.zip ../../Documents_OSX/
cd ../marmite
