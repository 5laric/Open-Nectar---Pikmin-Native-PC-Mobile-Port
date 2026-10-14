# Spike de transporte directo

Estado: Fase 1 en curso.

## Resultado Linux

- GameNetworkingSockets: commit `d534c19aa760df3fb75fd20db13ba1932b8a5463`.
- Licencia: BSD-3-Clause.
- Protobuf usado en el spike aislado: v21.12 (`f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c`).
- Criptografía: OpenSSL 3.5.5 del sistema.
- Configuración: biblioteca estática, examples/tests/tools desactivados, `ENABLE_ICE=OFF`.
- Resultado: `GameNetworkingSockets_s` compila correctamente con GCC 15.2.
- Prueba: servidor y cliente en procesos separados conectaron por UDP a `127.0.0.1:28960`, intercambiaron el handshake de GNS y cerraron limpiamente.

La prueba se hizo íntegramente bajo `/tmp`; no instaló paquetes ni copió dependencias al repositorio.

## Dependencias confirmadas

GNS necesita una implementación criptográfica y protobuf. `protoc` y el runtime deben coincidir. Desactivar ICE evita WebRTC/señalización, pero no elimina protobuf. La integración de release deberá fijar y construir de forma reproducible:

1. protobuf para el target y `protoc` para el host;
2. OpenSSL o una alternativa auditada compatible con cada plataforma;
3. GameNetworkingSockets con ICE, shared library, tools y examples desactivados.

No se aceptará que el ejecutable dependa de una DLL/.so no incluida en el paquete.

## Estado por plataforma

| Plataforma | Configuración | Build | Conexión directa | Estado |
|---|---:|---:|---:|---|
| Linux x86-64 | sí | sí | localhost sí | validación inicial completa |
| Windows x86-64/MinGW | pendiente | pendiente | pendiente | bloque de trabajo siguiente |
| Android arm64/NDK 28 | pendiente | pendiente | pendiente | riesgo principal del spike |

El README upstream afirma que la biblioteca se usa en plataformas móviles, pero también advierte que parte del código multiplataforma de Valve no se distribuye en el repositorio abierto. Por ello no se asumirá soporte Android hasta compilar, enlazar y conectar en un dispositivo real.

## Integración ya añadida al repositorio

- `libs/nectar_net`: biblioteca C++17 independiente del motor.
- `ITransport`: contrato de listen/connect/send/poll/close.
- Byte I/O big-endian acotado.
- Cabecera y tipos v0, decoder transaccional y errores explícitos.
- Golden vector y pruebas de truncado, versión, tipo, flags, tamaño y longitud.
- `NECTAR_ONLINE=OFF` por defecto en ambos juegos; al activarlo se enlaza `nectar_net`.

Validación ejecutada:

- configuración y build standalone de `nectar_net`: correcto;
- `nectar_net_protocol_test`: 1/1 correcto;
- ASan+UBSan: 1/1 correcto con detección de fugas desactivada porque LeakSanitizer no funciona bajo el `ptrace` del sandbox;
- configuración `NECTAR_ONLINE=ON` y build del target `nectar_net` desde Pikmin 1 y Pikmin 2: correcto.

## Trabajo pendiente para cerrar la Fase 1

1. fijar la estrategia reproducible de dependencias sin instalar paquetes globales;
2. implementar `GnsTransport` detrás de `ITransport`;
3. probar mensajes fiables/no fiables y métricas;
4. compilar y conectar Linux↔Windows, Linux↔Android y Windows↔Android;
5. revisar tamaño final, licencias, cierre, fugas y ciclo de vida Android.
