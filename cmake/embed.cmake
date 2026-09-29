# Transforme un fichier texte en tableau C : cmake -DIN=... -DOUT=... -DNAME=... -P embed.cmake
file(READ "${IN}" HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" BYTES "${BYTES}")
file(WRITE "${OUT}" "/* Généré automatiquement depuis ${IN} */\nstatic const char ${NAME}[] = {\n${BYTES}0x00};\n")
