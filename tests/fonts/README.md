# Test fonts

The faces the text goldens are rendered with (`tests/text_scenes.hpp`). The
oracle (`tools/painter_oracle`, `make painter-goldens`) and the render tests
both open these exact files, so the goldens do not depend on what a host has
installed.

| file | face | from | sha256 | licence |
|---|---|---|---|---|
| `NotoSans-Regular.ttf` | Noto Sans Regular | the DeBeOS image (hrev59996), `/boot/system/data/fonts/ttfonts` | `a52fdd375fb885e6ac21471b34e26e5cb6e58f1fa944519bff08d4400605bfed` | SIL OFL 1.1 (`licenses/SIL-OFL-1.1`) |
| `bchr.pfb` | Bitstream Charter Regular (Type 1) | the DeBeOS image, `/boot/system/data/fonts/psfonts` | `8ac5bad34cad9b59b087eebfaf9087c16901b0761f7f9cc538f2f65567fc3959` | Bitstream's grant (`licenses/Bitstream-Charter`); BITSTREAM CHARTER is a registered trademark of Bitstream Inc. |
| `DejaVuSans.ttf` | DejaVu Sans Book 2.37 | Amazon Linux 2023 `dejavu-sans-fonts-2.37` | `4383d51405d8ed99ff404fe546182aee4438781d16d6580e4a2e4eac95ba0db6` | Bitstream Vera / DejaVu licence (`licenses/DejaVu`) |
| `DejaVuSansMono.ttf` | DejaVu Sans Mono Book 2.37 | Amazon Linux 2023 `dejavu-sans-mono-fonts-2.37` | `a937745cb7dd35e819cb94b5e43e59e439ad3c4e1d37e94c219a29fcd6dfe431` | as above |

Unmodified copies. The licence texts are the ones Haiku ships in
`data/system/data/licenses` (OFL, Charter) and the one the DejaVu package ships.
