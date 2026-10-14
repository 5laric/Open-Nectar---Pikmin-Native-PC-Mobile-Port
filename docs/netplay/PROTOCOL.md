# Nectar Net Protocol v0

Estado: contrato previo a implementación. Los números son provisionales hasta congelar v1.

## Invariantes

- Orden de bytes de red (big-endian) para enteros multibyte.
- No se serializan structs C++, punteros, padding ni enums sin ancho.
- Todo mensaje se parsea y valida completamente antes de aplicarlo al juego.
- Longitud máxima de datagrama no fiable: 1.100 bytes.
- Longitud máxima de mensaje lógico fiable: 1 MiB; `bulk` fragmenta con un máximo total explícito.
- Un mensaje pertenece a una `session_id`, `scene_epoch` y versión de protocolo.
- El host es la única autoridad del mundo y del save.

## Cabecera v0

| Campo | Tipo | Validación |
|---|---:|---|
| magic | u32 | `0x4e435452` (`NCTR`) |
| protocol_major | u16 | igual en ambos peers |
| protocol_minor | u16 | negociado por capabilities |
| message_type | u8 | valor conocido |
| flags | u8 | bits desconocidos rechazados en v0 |
| channel_epoch | u16 | no menor que la época activa |
| session_id | u64 | igual a la sesión autenticada |
| scene_epoch | u32 | igual o transición permitida |
| sequence | u32 | ventana acotada por canal |
| ack_sequence | u32 | no futuro fuera de ventana |
| simulation_tick | u64 | ventana temporal acotada |
| payload_length | u16 | coincide con bytes restantes y límite del tipo |

## Canales lógicos

| Canal | Entrega | Mensajes iniciales |
|---|---|---|
| control | fiable/ordenada | hello, reglas, escenas, pausa, espectador, eventos críticos |
| input | no fiable/no ordenada | input actual más 3–5 ticks anteriores |
| state | no fiable/no ordenada | deltas y snapshots reemplazables |
| bulk | fiable/ordenada | snapshot completo y contenido aprobado |

## Handshake

1. El invitado decodifica el código y conecta a un endpoint.
2. El host responde a un `ClientProbe` pequeño con una cookie sin estado, sin amplificar bytes.
3. `ClientHello` demuestra posesión de `join_secret` y vincula el transcript a la clave/fingerprint del host.
4. Ambos negocian protocolo, juego, build, región, manifest de gameplay y capabilities.
5. El host envía `SessionRules`; el invitado acepta o rechaza explícitamente.
6. Tras autenticar se acuerdan `session_id` y un `resume_secret` distinto.
7. El host consume la plaza y deja de aceptar nuevos handshakes.

El código nunca es un token de sesión permanente. Checksum sólo detecta errores de copia; el MAC/fingerprint autentica.

## Tipos v0 reservados

| ID | Nombre | Canal | Dirección |
|---:|---|---|---|
| 1 | ClientProbe | control/preauth | C→H |
| 2 | HostCookie | control/preauth | H→C |
| 3 | ClientHello | control | C→H |
| 4 | HostHello | control | H→C |
| 5 | SessionRules | control | H→C |
| 6 | CompatibilityResult | control | ambas |
| 7 | Ping | control | ambas |
| 8 | Pong | control | ambas |
| 16 | InputBundle | input | C→H |
| 32 | WorldDelta | state | H→C |
| 33 | WorldSnapshot | bulk | H→C |
| 34 | StateHash | control | ambas |
| 48 | ScenePrepare | control | H→C |
| 49 | SceneReady | control | C→H |
| 50 | SceneStart | control | H→C |
| 64 | LeaveCoopRequest | control | C→H |
| 65 | RejoinRequest | control | C→H |
| 66 | PlayerControlChanged | control | H→C |
| 80 | ContentManifest | control | ambas |
| 81 | ContentDecision | control | C→H |
| 82 | ContentChunk | bulk | ambas |
| 96 | Disconnect | control | ambas |

## Límites v0

| Recurso | Límite inicial |
|---|---:|
| Peers autenticados | 2 total |
| Inputs por bundle | 6 |
| Datagrama no fiable | 1.100 bytes |
| Mensaje fiable normal | 64 KiB |
| Snapshot completo | 8 MiB comprimido / 32 MiB expandido |
| Paquete de personaje | 64 MiB total |
| Archivo de personaje | 32 MiB |
| Archivos por personaje | 128 |
| String de UI | 256 bytes UTF-8 |
| Intentos preauth por IP | presupuesto configurable y acotado |

Todos los límites se centralizarán en `nectar_net/limits.h` y reducirlos será compatible dentro de la misma versión menor; aumentarlos requiere capability.

## Compatibilidad

- `protocol_major` distinto: rechazo.
- Build o región distintos: rechazo inicial salvo pareja probada y allowlist explícita.
- Manifest de gameplay distinto: transferencia sólo para personajes declarativos admitidos; mods de lógica requieren coincidencia exacta.
- Campos desconocidos no se ignoran en v0 salvo que el mensaje declare una sección TLV opcional y acotada.

## Pendiente para v1

- Asignación definitiva de IDs y representación cuantizada.
- Esquemas por familia de entidad.
- Algoritmo exacto de MAC/cookie según la API final del transporte.
- Ventanas de secuencia, tick y reanudación medidas en pruebas de red.
