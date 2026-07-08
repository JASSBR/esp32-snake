/*
  ESP32-S3 WiFi Snake Arcade
  Wiring: GND→GND  VCC→3V3  SCL→GPIO9  SDA→GPIO8
  Connect: join WiFi "ESP32-SNAKE" → open 192.168.4.1
*/

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>

// ─── Hardware ─────────────────────────────────────────────────────────────────
#define SCREEN_W  128
#define SCREEN_H   64
#define SDA_PIN     8
#define SCL_PIN     9
#define OLED_ADDR 0x3C
#define RGB_PIN    48

// ─── Game grid (4px/cell → 32×16) ────────────────────────────────────────────
#define CELL  4
#define GW   (SCREEN_W/CELL)   // 32
#define GH   (SCREEN_H/CELL)   // 16
#define MAX_SNAKE (GW*GH)

#define AP_SSID "ESP32-SNAKE"
#define AP_PASS ""

// ─── Leaderboard ──────────────────────────────────────────────────────────────
#define LB_MAX 5
struct LBEntry { char name[16]; int score; };

// ─── Game types ───────────────────────────────────────────────────────────────
enum Dir   : uint8_t { R, L, U, D };
enum State : uint8_t { IDLE, PLAYING, DEAD };
struct Pt  { int8_t x, y; };

// ─── Game state ───────────────────────────────────────────────────────────────
Pt    body[MAX_SNAKE];
int   bodyLen = 0;
Pt    food    = {GW/2+5, GH/2};
bool  foodPower=false;
Dir   dir = R, nextDir = R;
State gameState = IDLE;
int   score=0, hiScore=0, tickMs=200;
bool  oledFound=false;
unsigned long lastTick=0, lastDraw=0;
LBEntry lb[LB_MAX]; int lbCount=0;

// ─── Peripherals ──────────────────────────────────────────────────────────────
Adafruit_SSD1306  oled(SCREEN_W, SCREEN_H, &Wire, -1);
AsyncWebServer    server(80);
AsyncWebSocket    ws("/ws");
Preferences       prefs;
Adafruit_NeoPixel led(1, RGB_PIN, NEO_GRB+NEO_KHZ800);
DNSServer         dns;

// ─── LED ──────────────────────────────────────────────────────────────────────
unsigned long ledFlashEnd=0;
void flashLED(uint32_t c,uint32_t ms){ ledFlashEnd=millis()+ms; led.setPixelColor(0,c); led.show(); }
void updateLED(){
  static unsigned long last=0; static uint8_t phase=0;
  if(millis()-last<20) return; last=millis();
  if(millis()<ledFlashEnd) return;
  if(gameState==IDLE||gameState==DEAD){
    phase++;
    led.setPixelColor(0,led.Color(0,(uint8_t)((sinf(phase*0.04f)+1.f)*18.f),0));
  } else { led.setPixelColor(0,0); }
  led.show();
}

// ─── Broadcast helpers ────────────────────────────────────────────────────────
void broadcastState() {
  if(!ws.count()) return;
  String m; m.reserve(bodyLen*8+100);
  m="{\"t\":\"s\",\"sc\":"; m+=score;
  m+=",\"hi\":"; m+=hiScore; m+=",\"tk\":"; m+=tickMs;
  m+=",\"pl\":"; m+=(int)ws.count(); m+=",\"sn\":[";
  for(int i=0;i<bodyLen;i++){if(i)m+=','; m+='[';m+=body[i].x;m+=',';m+=body[i].y;m+=']';}
  m+="],\"fd\":["; m+=food.x; m+=','; m+=food.y; m+="],\"fp\":"; m+=(foodPower?1:0);
  m+="}";
  ws.textAll(m);
}
void broadcastGameOver(){
  String m="{\"t\":\"go\",\"sc\":"; m+=score; m+=",\"hi\":"; m+=hiScore; m+="}";
  ws.textAll(m);
}
void broadcastLeaderboard(){
  String m="{\"t\":\"lb\",\"e\":[";
  for(int i=0;i<lbCount;i++){if(i)m+=','; m+="{\"n\":\"";m+=lb[i].name;m+="\",\"s\":";m+=lb[i].score;m+='}';}
  m+="]}"; ws.textAll(m);
}
void broadcastIdle(){
  String m="{\"t\":\"idle\",\"hi\":"; m+=hiScore; m+=",\"pl\":"; m+=(int)ws.count(); m+="}";
  ws.textAll(m);
}

// ─── Game logic ───────────────────────────────────────────────────────────────
void spawnFood(){
  for(int t=0;t<300;t++){
    Pt f={(int8_t)random(0,GW),(int8_t)random(0,GH)};
    bool hit=false;
    for(int i=0;i<bodyLen&&!hit;i++) hit=(body[i].x==f.x&&body[i].y==f.y);
    if(!hit){food=f;foodPower=(random(0,100)<25);return;}
  }
}
void startGame(){
  bodyLen=3;
  body[0]={(int8_t)(GW/2),(int8_t)(GH/2)};
  body[1]={(int8_t)(GW/2-1),(int8_t)(GH/2)};
  body[2]={(int8_t)(GW/2-2),(int8_t)(GH/2)};
  dir=R;nextDir=R;score=0;tickMs=200;gameState=PLAYING;
  spawnFood();
}
void addToLeaderboard(const char* name,int s){
  int pos=lbCount;
  for(int i=0;i<lbCount;i++){if(s>lb[i].score){pos=i;break;}}
  if(pos>=LB_MAX)return;
  int nc=min(lbCount+1,LB_MAX);
  for(int i=nc-1;i>pos;i--)lb[i]=lb[i-1];
  strncpy(lb[pos].name,name,15);lb[pos].name[15]=0;lb[pos].score=s;lbCount=nc;
  prefs.putInt("lbc",lbCount);
  for(int i=0;i<lbCount;i++){char k[8]; snprintf(k,8,"n%d",i);prefs.putString(k,lb[i].name); snprintf(k,8,"s%d",i);prefs.putInt(k,lb[i].score);}
}
void gameTick(){
  dir=nextDir;
  Pt h=body[0];
  switch(dir){case R:h.x++;break;case L:h.x--;break;case U:h.y--;break;case D:h.y++;break;}
  if(h.x<0||h.x>=GW||h.y<0||h.y>=GH){
    gameState=DEAD;if(score>hiScore){hiScore=score;prefs.putInt("hi",hiScore);}
    flashLED(led.Color(50,0,0),600);broadcastGameOver();return;
  }
  for(int i=0;i<bodyLen-1;i++){
    if(body[i].x==h.x&&body[i].y==h.y){
      gameState=DEAD;if(score>hiScore){hiScore=score;prefs.putInt("hi",hiScore);}
      flashLED(led.Color(50,0,0),600);broadcastGameOver();return;
    }
  }
  bool ate=(h.x==food.x&&h.y==food.y);
  int nl=bodyLen+(ate?1:0);
  for(int i=nl-1;i>0;i--)body[i]=body[i-1];
  body[0]=h;bodyLen=nl;
  if(ate){
    bool wasPower=foodPower;
    score+=wasPower?3:1;
    spawnFood();
    if(score%3==0&&tickMs>80)tickMs-=8;
    flashLED(wasPower?led.Color(50,40,0):led.Color(0,50,0),wasPower?150:80);
  }
  broadcastState();
}

// ─── OLED ─────────────────────────────────────────────────────────────────────
void drawOLED(){
  oled.clearDisplay();
  if(gameState==IDLE){
    oled.setTextSize(2);oled.setTextColor(SSD1306_WHITE);oled.setCursor(22,4);oled.print("SNAKE");
    oled.setTextSize(1);oled.setCursor(0,28);oled.print("WiFi: "AP_SSID);
    oled.setCursor(0,40);oled.print("Open: 192.168.4.1");
    oled.setCursor(0,54);oled.print("Players: ");oled.print((int)ws.count());
    oled.display();return;
  }
  if(gameState==DEAD){
    oled.setTextSize(2);oled.setTextColor(SSD1306_WHITE);oled.setCursor(16,4);oled.print("DEAD!");
    oled.setTextSize(1);oled.setCursor(0,30);oled.print("Score : ");oled.print(score);
    oled.setCursor(0,42);oled.print("Best  : ");oled.print(hiScore);oled.display();return;
  }
  if(!foodPower || (millis()/200)%2==0){
    if(foodPower) oled.drawRect(food.x*CELL,food.y*CELL,CELL,CELL,SSD1306_WHITE);
    oled.fillRect(food.x*CELL+1,food.y*CELL+1,2,2,SSD1306_WHITE);
  }
  for(int i=0;i<bodyLen;i++){
    int px=body[i].x*CELL,py=body[i].y*CELL;
    if(i==0)oled.fillRect(px,py,CELL,CELL,SSD1306_WHITE);
    else     oled.fillRect(px+1,py+1,CELL-2,CELL-2,SSD1306_WHITE);
  }
  oled.display();
}

// ─── WebSocket handler ────────────────────────────────────────────────────────
void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len){
  if(type==WS_EVT_CONNECT){
    Serial.printf("[ws] #%u connected\n",client->id());
    if(gameState==PLAYING) broadcastState(); else broadcastIdle();
    broadcastLeaderboard(); return;
  }
  if(type==WS_EVT_DISCONNECT){
    Serial.printf("[ws] #%u disconnected\n",client->id()); return;
  }
  if(type!=WS_EVT_DATA) return;
  AwsFrameInfo* info=(AwsFrameInfo*)arg;
  if(!info->final||info->index!=0||info->len!=len) return;

  char buf[256]; size_t n=len<255?len:255;
  memcpy(buf,data,n); buf[n]=0;
  Serial.printf("[ws] msg: %s\n",buf);

  StaticJsonDocument<256> doc;
  if(deserializeJson(doc,buf)!=DeserializationError::Ok) return;
  const char* t=doc["t"]|"";

  if(strcmp(t,"dir")==0&&gameState==PLAYING){
    const char* v=doc["v"]|"";
    if(strcmp(v,"R")==0&&dir!=L)nextDir=R;
    else if(strcmp(v,"L")==0&&dir!=R)nextDir=L;
    else if(strcmp(v,"U")==0&&dir!=D)nextDir=U;
    else if(strcmp(v,"D")==0&&dir!=U)nextDir=D;
  }
  else if(strcmp(t,"start")==0){
    if(gameState==IDLE||gameState==DEAD){startGame();broadcastState();}
  }
  else if(strcmp(t,"submit")==0&&gameState==DEAD){
    const char* name=doc["n"]|"ANON";
    addToLeaderboard(name,score);broadcastLeaderboard();
    gameState=IDLE;broadcastIdle();
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup(){
  Serial.begin(115200);
  led.begin();led.setBrightness(60);led.setPixelColor(0,led.Color(0,30,0));led.show();

  WiFi.softAP(AP_SSID,strlen(AP_PASS)?AP_PASS:nullptr);
  Serial.print("[wifi] "); Serial.println(WiFi.softAPIP());
  dns.start(53,"*",WiFi.softAPIP());

  Wire.begin(SDA_PIN,SCL_PIN);
  oledFound=oled.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR);
  if(oledFound){
    oled.clearDisplay();oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);oled.setCursor(0,0);oled.print("Booting...");oled.display();
  } else {
    Serial.printf("[oled] init FAILED at address 0x%02X\n",OLED_ADDR);
  }

  prefs.begin("snake",false);
  hiScore=prefs.getInt("hi",0);lbCount=prefs.getInt("lbc",0);
  for(int i=0;i<lbCount;i++){
    char k[8]; snprintf(k,8,"n%d",i);prefs.getString(k,lb[i].name,16);
    snprintf(k,8,"s%d",i);lb[i].score=prefs.getInt(k,0);
  }

  LittleFS.begin();

  // WebSocket registered FIRST, then explicit HTTP routes — NO serveStatic
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
    Serial.println("[http] GET /");
    req->send(LittleFS,"/index.html","text/html");
  });
  // Captive portal intercepts
  auto redir=[](AsyncWebServerRequest* req){ req->redirect("http://192.168.4.1/"); };
  server.on("/hotspot-detect.html",       HTTP_GET, redir);
  server.on("/library/test/success.html", HTTP_GET, redir);
  server.on("/generate_204",              HTTP_GET, redir);
  server.on("/gen_204",                   HTTP_GET, redir);
  server.on("/ncsi.txt",                  HTTP_GET, redir);
  server.on("/connecttest.txt",           HTTP_GET, redir);
  server.onNotFound([](AsyncWebServerRequest* req){ req->redirect("http://192.168.4.1/"); });

  server.begin();
  Serial.println("[ready] http:80 ws:/ws");
  drawOLED();
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop(){
  dns.processNextRequest();
  ws.cleanupClients();
  updateLED();

  unsigned long now=millis();
  if(gameState==PLAYING){
    if(now-lastTick>=(unsigned long)tickMs){lastTick=now;gameTick();drawOLED();}
  } else {
    if(now-lastDraw>2000){lastDraw=now;drawOLED();}
  }
}
