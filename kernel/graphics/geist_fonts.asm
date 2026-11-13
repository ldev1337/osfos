global geist_ttf_start
global geist_ttf_end
global geistmono_ttf_start
global geistmono_ttf_end

section .rodata
align 16

geist_ttf_start:
    incbin "assets/fonts/Geist.ttf"
geist_ttf_end:

geistmono_ttf_start:
    incbin "assets/fonts/GeistMono.ttf"
geistmono_ttf_end: