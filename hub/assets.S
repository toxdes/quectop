.section .rodata
.global asset_style, asset_style_end, asset_app, asset_app_end
.global asset_index, asset_index_end
asset_style:
.incbin ".build/style.css"
asset_style_end:
asset_app:
.incbin ".build/app.js"
asset_app_end:
asset_index:
.incbin ".build/index.html"
asset_index_end:
.section .note.GNU-stack,"",%progbits
