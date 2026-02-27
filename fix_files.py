
import os

def fix_file(filepath, replacements):
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    
    for target, replacement in replacements:
        content = content.replace(target, replacement)
    
    with open(filepath, 'w', encoding='utf-8', newline='') as f:
        f.write(content)

# ImageScreen.cpp fix
fix_file('src/ImageScreen.cpp', [
    ('static void pngDrawCallback(PNGDRAW *pDraw) {', 'static int pngDrawCallback(PNGDRAW *pDraw) {'),
    ('*pOut++ = (pixel & 0x1F) << 3;         // B\n  }\n}', '*pOut++ = (pixel & 0x1F) << 3;         // B\n  }\n  return 1;\n}')
])

# GDEP133C02.c fix
fix_file('src/GDEP133C02.c', [
    ('status == DONE', 'status == EPD_DONE'),
    ('status != DONE', 'status != EPD_DONE'),
    ('status = DONE', 'status = EPD_DONE'),
    ('status = ERROR', 'status = EPD_ERROR'),
    ('partialWindowUpdateStatus == DONE', 'partialWindowUpdateStatus == EPD_DONE'),
    ('partialWindowUpdateStatus = DONE', 'partialWindowUpdateStatus = EPD_DONE'),
    ('partialWindowUpdateStatus = ERROR', 'partialWindowUpdateStatus = EPD_ERROR'),
    ('printf("partialWindowUpdateStatus = ERROR \\r\\n");', 'printf("partialWindowUpdateStatus = EPD_ERROR \\r\\n");')
])

print("Fixes applied successfully")
