# Agente Inteligente de Automatización y Supervisión Empresarial (ESP32 + LLM)

Sistema embebido implementado en **ESP32 (C++ / Arduino)** que interpreta órdenes en lenguaje natural mediante la API de **Google Gemini**, valida estrictamente las acciones generadas en formato JSON y ejecuta tareas inmediatas, programadas o condicionales sobre actuadores y sensores físicos simulados.

---

## 1. Arquitectura del Sistema

```text
[Usuario (Web UI)] 
       │  (Lenguaje natural)
       ▼
[Servidor Web ESP32] ──(HTTPS POST)──► [API LLM (Gemini 2.5 Flash)]
       │                                         │
       │◄────────(JSON Estructurado)─────────────┘
       ▼
[Validador de Esquema C++] ──(Rechaza parámetros no autorizados)
       │
       ▼
[Planificador & Memoria Flash (Preferences)] ──(Persistencia ante reinicio)
       │
       ▼
[Catálogo Determinista C++] ──► Salidas (LEDs) / Entradas (Interruptores)
```

---

## 2. Mapa de Conexiones (Hardware ESP32 Clásica)

### Salidas Digitales (Actuadores - LEDs con resistencia de 220 Ω a GND)

| Dispositivo | ID Interno | Pin GPIO |
| :--- | :--- | :---: |
| Luz Entrada | `entrada` | **GPIO 4** |
| Luz Oficina | `oficina` | **GPIO 16** |
| Luz Parqueadero | `parqueadero` | **GPIO 17** |
| Luz Cocina | `cocina` | **GPIO 18** |
| Sistema Aspersores | `aspersores` | **GPIO 19** |

### Entradas Digitales (Sensores - Interruptores con `INPUT_PULLUP` hacia GND)
*Estado lógico:* `LOW` = Cerrado | `HIGH` = Abierto.

| Acceso | ID Interno | Pin GPIO |
| :--- | :--- | :---: |
| Puerta Principal | `puerta_principal` | **GPIO 25** |
| Puerta Trasera | `puerta_trasera` | **GPIO 26** |
| Ventana Oficina | `ventana_oficina` | **GPIO 27** |

---

## 3. Instalación y Configuración

1. Clonar este repositorio y abrir `Agente_ESP32_Clasica.ino` en **Arduino IDE**.
2. Instalar la librería **ArduinoJson** (v7+) desde el Gestor de Librerías.
3. Duplicar el archivo `credenciales.example.h`, renombrarlo a `credenciales.h` (ignorado por `.gitignore`) y completar los datos de red Wi-Fi y la clave de **Google AI Studio**.
4. Compilar y cargar en la placa **ESP32 Dev Module**.
5. Abrir el Monitor Serie a **115200 baudios** para obtener la dirección IP asignada y acceder desde el navegador web.

---

## 4. Formato JSON de Comunicación (LLM → ESP32)

El modelo de lenguaje actúa únicamente como traductor semántico y debe responder bajo el siguiente esquema validado localmente:

```json
{
  "estado": "valido",
  "mensaje": "Resumen legible de la operación",
  "acciones": [
    {
      "funcion": "encender",
      "dispositivo": "entrada",
      "condicion": "ninguna",
      "hora": "20:00",
      "offset_minutos": 0
    }
  ]
}
```

* **`estado`**: `"valido"`, `"ambiguo"` (solicita aclaración) o `"consulta"` (lee sensores locales).
* **`funcion`**: `"encender"`, `"apagar"`, `"consultar_accesos"`.
* **`dispositivo`**: `"entrada"`, `"oficina"`, `"parqueadero"`, `"cocina"`, `"todas_luces"`, `"aspersores"`, `"accesos"`.
* **`condicion`**: `"ninguna"`, `"si_puertas_cerradas"`, `"si_alguna_abierta"`.

---

## 5. Comportamiento ante Fallos y Reinicios

* **Sin conexión Wi-Fi:** El servidor web local y el planificador siguen operando con las tareas almacenadas. Las nuevas consultas al LLM devuelven una alerta controlada sin bloquear el microcontrolador.
* **Fallo de API o JSON inválido:** Si la API retorna un código distinto de HTTP 200 o parámetros fuera del catálogo autorizado, el firmware descarta la instrucción y notifica en pantalla sin alterar los pines.
* **Reinicio o corte de energía:** Las tareas pendientes se serializan en la memoria no volátil (`Preferences` / NVS) y se restauran automáticamente dentro de `setup()` antes de iniciar el ciclo principal.

---

## 6. Informe de Pruebas Funcionales

| Caso de Prueba | Instrucción / Evento | Resultado Esperado y Verificado |
| :--- | :--- | :--- |
| **1. Control inmediato y consulta** | *"¿Qué puertas o ventanas están abiertas?"* | Consulta los pines `25, 26, 27` en tiempo real y reporta exactamente los accesos en estado `HIGH`. |
| **2. Tarea horaria y secuencia** | *"Enciende la luz de la oficina a las 6:00 p. m. y quince minutos después la del parqueadero"* | Crea dos tareas independientes en cola (`18:00` con offset `0` y `18:00` con offset `+15`) y las guarda en Flash. |
| **3. Ejecución condicional** | *"Revisa las puertas y, si todas están cerradas, apaga todas las luces"* | Evalúa localmente `todasPuertasCerradas()`. Si un interruptor está abierto, cancela el apagado y emite alerta. |
| **4. Instrucción ambigua** | *"Enciende la luz más tarde"* | El LLM retorna `"estado": "ambiguo"`. El sistema no guarda ninguna tarea y solicita especificar la hora. |
| **5. Persistencia tras reinicio** | Desconectar y reconectar alimentación de la ESP32 con tareas pendientes | Al arrancar, `cargarTareasDeFlash()` restaura la cola completa y ejecuta las acciones al cumplirse la hora NTP. |
