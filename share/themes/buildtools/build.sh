if ! command -v sass 2>&1 >/dev/null
then
    echo "sass could not be found."
    echo "Install sass from https://github.com/sass/dart-sass/releases/ and add it to the \$PATH."
    exit 1
fi

mkdir -p ../Inkscape/gtk-4.0
sass -q --no-source-map ../src/Default-light.scss ../Inkscape/gtk-4.0/gtk.css
sass -q --no-source-map ../src/Default-dark.scss  ../Inkscape/gtk-4.0/gtk-dark.css
cp -r ../src/assets ../Inkscape/gtk-4.0

# test theme by installing it locally
cp -r ../Inkscape ~/.local/share/themes
