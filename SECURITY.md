# Seguridad

## Cómo se construye cada versión

- Los ejecutables y el instalador se compilan en **GitHub Actions** ([build.yml](.github/workflows/build.yml)) a partir del código de este repositorio, sin pasos manuales.
- El `.exe` no usa empaquetadores ni ofuscación. Lleva datos de versión y tiene ASLR, DEP y Control Flow Guard activados.
- Cada versión publica `SHA256SUMS.txt` con las sumas de todos los archivos.
- Antes de publicarse, cada versión se escanea con Microsoft Defender.

## Lo que Zyntra no hace

No pide permisos de administrador, no se ejecuta al iniciar Windows, no instala servicios ni controladores,
no inyecta código en otros procesos y no se conecta a otros servidores que los indicados en [PRIVACIDAD.txt](docs/PRIVACIDAD.txt).

## Falsos positivos de antivirus

Los programas nuevos y sin firma digital pueden recibir alertas por falta de «reputación», aunque no sean dañinos.
Si tu antivirus marca Zyntra:

1. Comprueba que lo descargaste del repositorio oficial y que la suma SHA-256 coincide.
2. Abre un [issue](../../issues) con el nombre del antivirus y del aviso.
3. Lo reportaremos como falso positivo al fabricante (por ejemplo, Microsoft: https://www.microsoft.com/wdsi/filesubmission).

## Reportar una vulnerabilidad

Usa **Security → Report a vulnerability** en este repositorio (aviso privado) o abre un issue sin detalles explotables.
Respondemos lo antes posible.
