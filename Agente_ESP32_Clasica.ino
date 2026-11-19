#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

#if __has_include("credenciales.h")
  #include "credenciales.h"
#else
  #define WIFI_NOMBRE    "NombreDeTuRed"
  #define WIFI_CLAVE     "ClaveDeTuRed"
  #define GEMINI_API_KEY "PON_TU_API_KEY_AQUI"
#endif

// =============================================================================
// 1. CONFIGURACIÓN DE HARDWARE (PINES SEGUROS PARA ESP32 CLÁSICA)
// =============================================================================
// Salidas digitales (LEDs simulando luces y aspersores)
// Se usan GPIOs seguros de salida sin conflictos de boot (4, 16, 17, 18, 19)
struct Actuador {
  const char* id;
  const char* nombre;
  const char* tipo; // "luz" o "aspersor"
  int pin;
  bool estado;
};

Actuador ACTUADORES[] = {
  {"entrada",      "Luz Entrada",        "luz",      4,  false},
  {"oficina",      "Luz Oficina",        "luz",      16, false},
  {"parqueadero",  "Luz Parqueadero",    "luz",      17, false},
  {"cocina",       "Luz Cocina",         "luz",      18, false},
  {"aspersores",   "Sistema Aspersores", "aspersor", 19, false}
};
const int NUM_ACTUADORES = sizeof(ACTUADORES) / sizeof(ACTUADORES);

// Entradas digitales (Interruptores simulando puertas y ventanas con INPUT_PULLUP)
// Pines 25, 26, 27 tienen pull-up interno real en ESP32 clásica (no usar 34-39)
// LOW = Cerrado (conectado a GND), HIGH = Abierto
struct SensorAcceso {
  const char* id;
  const char* nombre;
  const char* tipo; // "puerta" o "ventana"
  int pin;
  bool abierto;
};

SensorAcceso SENSORES[] = {
  {"puerta_principal", "Puerta Principal", "puerta",  25, false},
  {"puerta_trasera",   "Puerta Trasera",   "puerta",  26, false},
  {"ventana_oficina",  "Ventana Oficina",  "ventana", 27, false}
};
const int NUM_SENSORES = sizeof(SENSORES) / sizeof(SENSORES);

// =============================================================================
// 2. HORA NTP Y PERSISTENCIA DE TAREAS
// =============================================================================
const char* SERVIDOR_NTP = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -5 * 3600; // Colombia (UTC-5)
const int   DAYLIGHT_OFFSET_SEC = 0;

struct Tarea {
  bool activa;
  int id;
  String funcion;      // "encender", "apagar", "consultar_accesos"
  String dispositivo;  // "entrada", "oficina", "todas_luces", "aspersores", "accesos"
  String condicion;    // "ninguna", "si_puertas_cerradas", "si_alguna_abierta"
  time_t ejecutarEn;   // Timestamp Unix exacto de ejecución
  String horaTexto;    // Para mostrar en interfaz
};

const int MAX_TAREAS = 12; // Optimizado para RAM de ESP32 clásica
Tarea colaTareas[MAX_TAREAS];
int contadorIdTareas = 1;

Preferences memoria;
WebServer servidor(80);
String ultimaAlerta = "Sistema iniciado correctamente en ESP32.";

// =============================================================================
// 3. CATÁLOGO DE FUNCIONES C++ AUTORIZADAS (EJECUCIÓN DETERMINISTA)
// =============================================================================
void actualizarSensores() {
  for (int i = 0; i < NUM_SENSORES; i++) {
    SENSORES[i].abierto = (digitalRead(SENSORES[i].pin) == HIGH);
  }
}

bool todasPuertasCerradas() {
  actualizarSensores();
  for (int i = 0; i < NUM_SENSORES; i++) {
    if (String(SENSORES[i].tipo) == "puerta" && SENSORES[i].abierto) {
      return false;
    }
  }
  return true;
}

String obtenerReporteAccesos() {
  actualizarSensores();
  String abiertos = "";
  for (int i = 0; i < NUM_SENSORES; i++) {
    if (SENSORES[i].abierto) {
      if (abiertos.length() > 0) abiertos += ", ";
      abiertos += SENSORES[i].nombre;
    }
  }
  if (abiertos.length() == 0) return "Todas las puertas y ventanas están cerradas.";
  return "ALERTA DE ACCESO - Abiertos: " + abiertos;
}

bool controlarActuador(const String& disp, bool encender) {
  bool encontrado = false;
  for (int i = 0; i < NUM_ACTUADORES; i++) {
    bool coincide = (disp == ACTUADORES[i].id) ||
                    (disp == "todas_luces" && String(ACTUADORES[i].tipo) == "luz");
    if (coincide) {
      ACTUADORES[i].estado = encender;
      digitalWrite(ACTUADORES[i].pin, encender ? HIGH : LOW);
      encontrado = true;
    }
  }
  return encontrado;
}

void ejecutarFuncionAutorizada(const Tarea& t) {
  if (t.condicion == "si_puertas_cerradas") {
    if (!todasPuertasCerradas()) {
      ultimaAlerta = "Tarea #" + String(t.id) + " cancelada: hay al menos una puerta abierta.";
      Serial.println("[PLANIFICADOR] " + ultimaAlerta);
      return;
    }
  }

  if (t.funcion == "encender") {
    controlarActuador(t.dispositivo, true);
    ultimaAlerta = "Ejecutado: Encendido -> " + t.dispositivo;
  } else if (t.funcion == "apagar") {
    controlarActuador(t.dispositivo, false);
    ultimaAlerta = "Ejecutado: Apagado -> " + t.dispositivo;
  } else if (t.funcion == "consultar_accesos") {
    String rep = obtenerReporteAccesos();
    if (t.condicion == "si_alguna_abierta" && rep.startsWith("Todas")) {
      ultimaAlerta = "Verificación programada: Todo cerrado sin novedad.";
    } else {
      ultimaAlerta = rep;
    }
  }
  Serial.println("[EJECUTADO] " + ultimaAlerta);
}

// =============================================================================
// 4. PERSISTENCIA EN MEMORIA NO VOLÁTIL (PREFERENCES)
// =============================================================================
void guardarTareasEnFlash() {
  memoria.begin("agente_tareas", false);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < MAX_TAREAS; i++) {
    if (colaTareas[i].activa) {
      JsonObject obj = arr.add<JsonObject>();
      obj["id"] = colaTareas[i].id;
      obj["fn"] = colaTareas[i].funcion;
      obj["dp"] = colaTareas[i].dispositivo;
      obj["cd"] = colaTareas[i].condicion;
      obj["ts"] = (long)colaTareas[i].ejecutarEn;
      obj["hr"] = colaTareas[i].horaTexto;
    }
  }
  String serializado;
  serializeJson(doc, serializado);
  memoria.putString("cola", serializado);
  memoria.putInt("nextId", contadorIdTareas);
  memoria.end();
}

void cargarTareasDeFlash() {
  memoria.begin("agente_tareas", true);
  String serializado = memoria.getString("cola", "[]");
  contadorIdTareas = memoria.getInt("nextId", 1);
  memoria.end();

  JsonDocument doc;
  if (deserializeJson(doc, serializado)) return;

  int idx = 0;
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (idx >= MAX_TAREAS) break;
    colaTareas[idx].activa      = true;
    colaTareas[idx].id          = obj["id"];
    colaTareas[idx].funcion     = obj["fn"].as<String>();
    colaTareas[idx].dispositivo = obj["dp"].as<String>();
    colaTareas[idx].condicion   = obj["cd"].as<String>();
    colaTareas[idx].ejecutarEn  = (time_t)obj["ts"].as<long>();
    colaTareas[idx].horaTexto   = obj["hr"].as<String>();
    idx++;
  }
  Serial.printf("[PERSISTENCIA] %d tareas recuperadas tras arranque.\n", idx);
}

// =============================================================================
// 5. VALIDACIÓN ESTRICTA DE ESQUEMA Y PLANIFICADOR
// =============================================================================
bool esFuncionValida(const String& f) {
  return (f == "encender" || f == "apagar" || f == "consultar_accesos");
}

bool esDispositivoValido(const String& d) {
  if (d == "todas_luces" || d == "accesos") return true;
  for (int i = 0; i < NUM_ACTUADORES; i++) {
    if (d == ACTUADORES[i].id) return true;
  }
  return false;
}

bool esCondicionValida(const String& c) {
  return (c == "ninguna" || c == "si_puertas_cerradas" || c == "si_alguna_abierta");
}

time_t calcularTimestamp(const String& horaStr, int offsetMinutos) {
  time_t ahora;
  time(&ahora);
  if (horaStr == "ahora" || horaStr == "") {
    return ahora + (offsetMinutos * 60);
  }
  int sep = horaStr.indexOf(':');
  if (sep < 0) return 0;
  int hh = horaStr.substring(0, sep).toInt();
  int mm = horaStr.substring(sep + 1).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return 0;

  struct tm infoTiempo;
  localtime_r(&ahora, &infoTiempo);
  infoTiempo.tm_hour = hh;
  infoTiempo.tm_min  = mm;
  infoTiempo.tm_sec  = 0;

  time_t objetivo = mktime(&infoTiempo);
  if (objetivo <= ahora && offsetMinutos == 0) {
    objetivo += 86400;
  }
  return objetivo + (offsetMinutos * 60);
}

String procesarPlanJSON(const String& jsonStr) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    return "Error: El LLM devolvió un formato inválido (no es JSON).";
  }

  String estado = doc["estado"] | "invalido";
  String mensajeLLM = doc["mensaje"] | "Sin respuesta.";

  if (estado == "ambiguo" || estado == "consulta") {
    if (estado == "consulta") {
      String rep = obtenerReporteAccesos();
      return mensajeLLM + " | Estado actual: " + rep;
    }
    return "⚠️ Aclaración requerida: " + mensajeLLM;
  }

  if (estado != "valido" || !doc["acciones"].is<JsonArray>()) {
    return "Rechazado: Esquema de plan no autorizado.";
  }

  JsonArray acciones = doc["acciones"].as<JsonArray>();
  if (acciones.size() == 0) return "No se detectaron acciones ejecutables.";

  int programadas = 0;
  for (JsonObject acc : acciones) {
    String fn = acc["funcion"] | "";
    String dp = acc["dispositivo"] | "";
    String cd = acc["condicion"] | "ninguna";
    String hr = acc["hora"] | "ahora";
    int offset = acc["offset_minutos"] | 0;

    if (!esFuncionValida(fn) || !esDispositivoValido(dp) || !esCondicionValida(cd)) {
      Serial.println("[VALIDACIÓN] Acción rechazada por parámetros fuera de catálogo.");
      continue;
    }

    time_t ts = calcularTimestamp(hr, offset);
    if (ts == 0) continue;

    if (hr == "ahora" && offset == 0) {
      Tarea t = {true, 0, fn, dp, cd, ts, "Inmediata"};
      ejecutarFuncionAutorizada(t);
      programadas++;
    } else {
      for (int i = 0; i < MAX_TAREAS; i++) {
        if (!colaTareas[i].activa) {
          colaTareas[i].activa      = true;
          colaTareas[i].id          = contadorIdTareas++;
          colaTareas[i].funcion     = fn;
          colaTareas[i].dispositivo = dp;
          colaTareas[i].condicion   = cd;
          colaTareas[i].ejecutarEn  = ts;
          colaTareas[i].horaTexto   = (hr == "ahora" ? "En " + String(offset) + "m" : hr + (offset > 0 ? " +" + String(offset) + "m" : ""));
          programadas++;
          break;
        }
      }
    }
  }
  guardarTareasEnFlash();
  return "✅ " + mensajeLLM + " (" + String(programadas) + " operación(es) procesada(s)).";
}

void revisarPlanificador() {
  time_t ahora;
  time(&ahora);
  bool huboCambios = false;

  for (int i = 0; i < MAX_TAREAS; i++) {
    if (colaTareas[i].activa && ahora >= colaTareas[i].ejecutarEn) {
      ejecutarFuncionAutorizada(colaTareas[i]);
      colaTareas[i].activa = false;
      huboCambios = true;
    }
  }
  if (huboCambios) guardarTareasEnFlash();
}

// =============================================================================
// 6. COMUNICACIÓN HTTPS CON EL LLM (GEMINI API - OPTIMIZADO PARA ESP32 CLÁSICA)
// =============================================================================
String consultarLLM(const String& ordenUsuario) {
  if (WiFi.status() != WL_CONNECTED) {
    return "{\"estado\":\"invalido\",\"mensaje\":\"Sin conexión Wi-Fi. Las tareas locales siguen activas.\"}";
  }

  WiFiClientSecure client;
  client.setInsecure(); // Reduce consumo de RAM al no cargar cadena de certificados CA

  HTTPClient http;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=" + String(GEMINI_API_KEY);
  
  if (!http.begin(client, url)) {
    return "{\"estado\":\"invalido\",\"mensaje\":\"Error iniciando conexión HTTPS.\"}";
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  struct tm timeinfo;
  char horaActual[10] = "00:00";
  if (getLocalTime(&timeinfo)) {
    strftime(horaActual, sizeof(horaActual), "%H:%M", &timeinfo);
  }

  String systemPrompt =
    "Eres el intérprete de un agente embebido ESP32. Hora actual: " + String(horaActual) + ". "
    "Traduce la orden del usuario ESTRICTAMENTE a un objeto JSON sin markdown ni texto extra. "
    "Catálogo permitido: "
    "funcion: ['encender', 'apagar', 'consultar_accesos']. "
    "dispositivo: ['entrada', 'oficina', 'parqueadero', 'cocina', 'todas_luces', 'aspersores', 'accesos']. "
    "condicion: ['ninguna', 'si_puertas_cerradas', 'si_alguna_abierta']. "
    "hora: 'ahora' o formato 24h 'HH:MM'. offset_minutos: entero (ej. 15 para 'quince minutos después'). "
    "Si la orden es ambigua (ej. 'más tarde', 'luego', dispositivo inexistente), responde: "
    "{\"estado\":\"ambiguo\",\"mensaje\":\"pregunta de aclaración\"}."
    "Si pregunta qué puertas/ventanas están abiertas o estado actual, responde: "
    "{\"estado\":\"consulta\",\"mensaje\":\"Consultando sensores...\"}."
    "Si es válida, responde: "
    "{\"estado\":\"valido\",\"mensaje\":\"resumen breve\",\"acciones\":[{\"funcion\":\"...\",\"dispositivo\":\"...\",\"condicion\":\"...\",\"hora\":\"...\",\"offset_minutos\":0}]}";

  JsonDocument payload;
  payload["system_instruction"]["parts"][0]["text"] = systemPrompt;
  payload["contents"][0]["parts"][0]["text"] = ordenUsuario;
  payload["generationConfig"]["responseMimeType"] = "application/json";
  payload["generationConfig"]["temperature"] = 0.1;

  String body;
  serializeJson(payload, body);

  int httpCode = http.POST(body);
  String respuestaLimpia = "{\"estado\":\"invalido\",\"mensaje\":\"Fallo en API LLM (HTTP " + String(httpCode) + ")\"}";

  if (httpCode == 200) {
    String raw = http.getString();
    JsonDocument resDoc;
    if (!deserializeJson(resDoc, raw)) {
      const char* textoIA = resDoc["candidates"][0]["content"]["parts"][0]["text"];
      if (textoIA) respuestaLimpia = String(textoIA);
    }
  }
  http.end();
  return respuestaLimpia;
}

// =============================================================================
// 7. INTERFAZ WEB ALOJADA EN ESP32 (EN MEMORIA FLASH PROGMEM)
// =============================================================================
const char HTML_INTERFAZ[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Agente Empresarial ESP32</title>
<style>
  body { font-family: system-ui, sans-serif; background: #f0f2f5; margin: 0; padding: 16px; color: #1f2937; }
  .wrap { max-width: 850px; margin: auto; }
  .card { background: #fff; border-radius: 10px; padding: 16px; margin-bottom: 14px; box-shadow: 0 1px 3px #0001; }
  h1 { font-size: 20px; margin: 0 0 12px; color: #0f766e; }
  h2 { font-size: 15px; margin: 0 0 10px; border-bottom: 1px solid #e5e7eb; padding-bottom: 6px; }
  .alerta { background: #fef3c7; border-left: 4px solid #d97706; padding: 10px; border-radius: 4px; font-weight: 500; margin-bottom: 12px; }
  .row { display: flex; gap: 8px; }
  input { flex: 1; padding: 10px; border: 1px solid #cbd5e1; border-radius: 6px; font-size: 15px; }
  button { background: #0f766e; color: #fff; border: none; padding: 10px 16px; border-radius: 6px; cursor: pointer; font-weight: bold; }
  button.del { background: #dc2626; padding: 4px 8px; font-size: 12px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 8px; }
  .item { padding: 10px; border-radius: 6px; border: 1px solid #e2e8f0; text-align: center; font-size: 14px; }
  .on { background: #dcfce7; border-color: #16a34a; color: #166534; font-weight: bold; }
  .abierto { background: #fee2e2; border-color: #dc2626; color: #991b1b; font-weight: bold; }
  table { width: 100%; border-collapse: collapse; font-size: 13px; }
  th, td { text-align: left; padding: 6px; border-bottom: 1px solid #e5e7eb; }
  #chat { background: #f8fafc; padding: 10px; border-radius: 6px; max-height: 160px; overflow-y: auto; font-size: 14px; margin-top: 10px; white-space: pre-line; }
</style>
</head>
<body>
<div class="wrap">
  <h1>🤖 Agente de Automatización Empresarial (ESP32 + LLM)</h1>
  <div class="alerta" id="alerta">Cargando estado del sistema...</div>

  <div class="card">
    <h2>💬 Instrucción en Lenguaje Natural</h2>
    <div class="row">
      <input id="orden" placeholder="Ej: A las 8:00 p. m. enciende la luz de la entrada y los aspersores..." onkeydown="if(event.key==='Enter')enviar()">
      <button onclick="enviar()" id="btnEnviar">Enviar</button>
    </div>
    <div id="chat">Escribe una orden o pregunta sobre el estado de accesos y tareas.</div>
  </div>

  <div class="card">
    <h2>💡 Dispositivos (Salidas Digitales)</h2>
    <div class="grid" id="actuadores"></div>
  </div>

  <div class="card">
    <h2>🚪 Puertas y Ventanas (Entradas Digitales)</h2>
    <div class="grid" id="sensores"></div>
  </div>

  <div class="card">
    <h2>⏰ Tareas Programadas (Memoria Persistente) — Hora ESP32: <span id="hora">--:--</span></h2>
    <table><thead><tr><th>ID</th><th>Acción</th><th>Dispositivo</th><th>Condición</th><th>Programada</th><th></th></tr></thead>
    <tbody id="tareas"></tbody></table>
  </div>
</div>

<script>
const enviar = async () => {
  const inp = document.getElementById("orden");
  const btn = document.getElementById("btnEnviar");
  const txt = inp.value.trim();
  if (!txt) return;
  inp.value = ""; btn.disabled = true; btn.textContent = "Pensando...";
  const chat = document.getElementById("chat");
  chat.textContent += "\n🧑 Tú: " + txt;
  try {
    const r = await fetch("/api/orden", { method: "POST", body: JSON.stringify({ texto: txt }) });
    const d = await r.json();
    chat.textContent += "\n🤖 Agente: " + d.respuesta + "\n";
    chat.scrollTop = chat.scrollHeight;
    actualizar();
  } catch (e) { chat.textContent += "\n❌ Error de comunicación.\n"; }
  btn.disabled = false; btn.textContent = "Enviar";
};

const cancelar = async (id) => {
  await fetch("/api/cancelar?id=" + id, { method: "POST" });
  actualizar();
};

const actualizar = async () => {
  const r = await fetch("/api/estado");
  const d = await r.json();
  document.getElementById("hora").textContent = d.hora;
  document.getElementById("alerta").textContent = "🔔 " + d.alerta;

  document.getElementById("actuadores").innerHTML = d.actuadores.map(a =>
    `<div class="item ${a.estado ? 'on' : ''}">${a.estado ? '🟢' : '⚫'} ${a.nombre}<br><small>GPIO ${a.pin}</small></div>`).join("");

  document.getElementById("sensores").innerHTML = d.sensores.map(s =>
    `<div class="item ${s.abierto ? 'abierto' : 'on'}">${s.abierto ? '🔓 ABIERTA' : '🔒 CERRADA'}<br>${s.nombre}<br><small>GPIO ${s.pin}</small></div>`).join("");

  document.getElementById("tareas").innerHTML = d.tareas.length ? d.tareas.map(t =>
    `<tr><td>#${t.id}</td><td>${t.funcion}</td><td>${t.dispositivo}</td><td>${t.condicion}</td><td>${t.hora}</td>
     <td><button class="del" onclick="cancelar(${t.id})">Cancelar</button></td></tr>`).join("")
    : "<tr><td colspan='6'>No hay tareas pendientes en memoria.</td></tr>";
};

actualizar();
setInterval(actualizar, 2500);
</script>
</body>
</html>
)HTML";

// =============================================================================
// 8. ENDPOINTS DEL SERVIDOR WEB
// =============================================================================
void handleEstado() {
  actualizarSensores();
  JsonDocument doc;
  
  struct tm timeinfo;
  char bufHora[12] = "Sin NTP";
  if (getLocalTime(&timeinfo)) strftime(bufHora, sizeof(bufHora), "%H:%M:%S", &timeinfo);
  doc["hora"] = bufHora;
  doc["alerta"] = ultimaAlerta;

  JsonArray arrAct = doc["actuadores"].to<JsonArray>();
  for (int i = 0; i < NUM_ACTUADORES; i++) {
    JsonObject o = arrAct.add<JsonObject>();
    o["nombre"] = ACTUADORES[i].nombre;
    o["pin"]    = ACTUADORES[i].pin;
    o["estado"] = ACTUADORES[i].estado;
  }

  JsonArray arrSen = doc["sensores"].to<JsonArray>();
  for (int i = 0; i < NUM_SENSORES; i++) {
    JsonObject o = arrSen.add<JsonObject>();
    o["nombre"]  = SENSORES[i].nombre;
    o["pin"]     = SENSORES[i].pin;
    o["abierto"] = SENSORES[i].abierto;
  }

  JsonArray arrTar = doc["tareas"].to<JsonArray>();
  for (int i = 0; i < MAX_TAREAS; i++) {
    if (colaTareas[i].activa) {
      JsonObject o = arrTar.add<JsonObject>();
      o["id"]          = colaTareas[i].id;
      o["funcion"]     = colaTareas[i].funcion;
      o["dispositivo"] = colaTareas[i].dispositivo;
      o["condicion"]   = colaTareas[i].condicion;
      o["hora"]        = colaTareas[i].horaTexto;
    }
  }
  String res;
  serializeJson(doc, res);
  servidor.send(200, "application/json", res);
}

void handleOrden() {
  JsonDocument req;
  if (deserializeJson(req, servidor.arg("plain")) || !req["texto"].is<const char*>()) {
    servidor.send(400, "application/json", "{\"respuesta\":\"Petición inválida.\"}");
    return;
  }
  String orden = req["texto"].as<String>();
  Serial.println("\n[USUARIO] " + orden);

  String jsonLLM = consultarLLM(orden);
  Serial.println("[LLM JSON] " + jsonLLM);

  String resultado = procesarPlanJSON(jsonLLM);
  Serial.println("[SISTEMA] " + resultado);

  JsonDocument res;
  res["respuesta"] = resultado;
  String out;
  serializeJson(res, out);
  servidor.send(200, "application/json", out);
}

void handleCancelar() {
  if (servidor.hasArg("id")) {
    int idBuscado = servidor.arg("id").toInt();
    for (int i = 0; i < MAX_TAREAS; i++) {
      if (colaTareas[i].activa && colaTareas[i].id == idBuscado) {
        colaTareas[i].activa = false;
        guardarTareasEnFlash();
        ultimaAlerta = "Tarea #" + String(idBuscado) + " cancelada por el usuario.";
        break;
      }
    }
  }
  servidor.send(200, "application/json", "{\"ok\":true}");
}

// =============================================================================
// 9. SETUP Y LOOP PRINCIPAL
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // Configurar salidas (LEDs)
  for (int i = 0; i < NUM_ACTUADORES; i++) {
    pinMode(ACTUADORES[i].pin, OUTPUT);
    digitalWrite(ACTUADORES[i].pin, LOW);
  }

  // Configurar entradas (Interruptores con resistencia pull-up interna)
  for (int i = 0; i < NUM_SENSORES; i++) {
    pinMode(SENSORES[i].pin, INPUT_PULLUP);
  }

  // Recuperar tareas pendientes de la memoria flash antes de conectar
  cargarTareasDeFlash();

  // Conectar Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
  Serial.print("Conectando a Wi-Fi");
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Conectado! IP: http://" + WiFi.localIP().toString());
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, SERVIDOR_NTP);
  } else {
    Serial.println("Advertencia: Sin Wi-Fi. Ejecutando en modo local de emergencia.");
  }

  servidor.on("/", HTTP_GET, []() { servidor.send_P(200, "text/html", HTML_INTERFAZ); });
  servidor.on("/api/estado", HTTP_GET, handleEstado);
  servidor.on("/api/orden", HTTP_POST, handleOrden);
  servidor.on("/api/cancelar", HTTP_POST, handleCancelar);
  servidor.begin();
}

void loop() {
  servidor.handleClient();
  revisarPlanificador();
}
