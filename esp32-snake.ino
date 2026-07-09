/*
  ESP32-S3 WiFi Snake Arcade
  Wiring: GND→GND  VCC→3V3  SCL→GPIO9  SDA→GPIO8
  Connect: joins your home WiFi (see secrets.h) → open the IP shown on the OLED
*/

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include "secrets.h"

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

// ─── Leaderboard ──────────────────────────────────────────────────────────────
#define LB_MAX 5
struct LBEntry { char name[16]; int score; };

// ─── Game types ───────────────────────────────────────────────────────────────
enum Dir   : uint8_t { R, L, U, D };
enum State : uint8_t { IDLE, PLAYING, DEAD };
struct Pt  { int8_t x, y; };

// ─── Players ──────────────────────────────────────────────────────────────────
#define NUM_PLAYERS 2
struct Player {
  Pt   body[MAX_SNAKE];
  int  len=0;
  Dir  dir=R, nextDir=R;
  bool alive=false;
  int  score=0;
  uint32_t clientId=0;
  bool connected=false;
};
Player players[NUM_PLAYERS];
bool   submitted[NUM_PLAYERS];

// ─── Game state ───────────────────────────────────────────────────────────────
Pt    food    = {GW/2+5, GH/2};
bool  foodPower=false;
State gameState = IDLE;
int   hiScore=0, tickMs=200;
bool  oledFound=false;
unsigned long lastTick=0, lastDraw=0;
LBEntry lb[LB_MAX]; int lbCount=0;

int playerSlotFor(uint32_t id){
  for(int i=0;i<NUM_PLAYERS;i++) if(players[i].connected&&players[i].clientId==id) return i;
  return -1;
}
int assignSlot(uint32_t id){
  for(int i=0;i<NUM_PLAYERS;i++) if(!players[i].connected){ players[i].connected=true; players[i].clientId=id; return i; }
  return -1;
}

// ─── Peripherals ──────────────────────────────────────────────────────────────
Adafruit_SSD1306  oled(SCREEN_W, SCREEN_H, &Wire, -1);
AsyncWebServer    server(80);
AsyncWebSocket    ws("/ws");
Preferences       prefs;
Adafruit_NeoPixel led(1, RGB_PIN, NEO_GRB+NEO_KHZ800);

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
  String m; m.reserve(400);
  m="{\"t\":\"s\",\"hi\":"; m+=hiScore; m+=",\"tk\":"; m+=tickMs;
  m+=",\"pl\":"; m+=(int)ws.count();
  m+=",\"fd\":["; m+=food.x; m+=','; m+=food.y; m+="],\"fp\":"; m+=(foodPower?1:0);
  m+=",\"p\":[";
  for(int i=0;i<NUM_PLAYERS;i++){
    if(i)m+=',';
    m+="{\"sc\":"; m+=players[i].score; m+=",\"al\":"; m+=(players[i].alive?1:0); m+=",\"sn\":[";
    for(int j=0;j<players[i].len;j++){if(j)m+=','; m+='[';m+=players[i].body[j].x;m+=',';m+=players[i].body[j].y;m+=']';}
    m+="]}";
  }
  m+="]}";
  ws.textAll(m);
}
void broadcastGameOver(){
  String m="{\"t\":\"go\",\"hi\":"; m+=hiScore; m+=",\"p\":[";
  for(int i=0;i<NUM_PLAYERS;i++){
    if(i)m+=',';
    m+="{\"sc\":"; m+=players[i].score; m+=",\"al\":"; m+=(players[i].alive?1:0);
    m+=",\"in\":"; m+=(players[i].len>0?1:0); m+='}';
  }
  m+="]}";
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
bool cellBlocked(Pt f){
  for(int i=0;i<NUM_PLAYERS;i++)
    for(int j=0;j<players[i].len;j++)
      if(players[i].body[j].x==f.x&&players[i].body[j].y==f.y) return true;
  return false;
}
void spawnFood(){
  for(int t=0;t<300;t++){
    Pt f={(int8_t)random(0,GW),(int8_t)random(0,GH)};
    if(!cellBlocked(f)){food=f;foodPower=(random(0,100)<25);return;}
  }
}
void resetPlayer(int i,int startX,int startY,Dir d){
  Player&p=players[i];
  p.len=3;p.dir=d;p.nextDir=d;p.alive=true;p.score=0;
  int8_t dx=(d==R)?-1:(d==L)?1:0;
  int8_t dy=(d==D)?-1:(d==U)?1:0;
  for(int k=0;k<3;k++) p.body[k]={(int8_t)(startX+dx*k),(int8_t)(startY+dy*k)};
}
void startGame(){
  resetPlayer(0,GW/4,GH/2-2,R);
  if(players[1].connected) resetPlayer(1,GW*3/4,GH/2+2,L);
  else { players[1].len=0; players[1].alive=false; }
  submitted[0]=false; submitted[1]=false;
  tickMs=200;gameState=PLAYING;
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
  Pt newHead[NUM_PLAYERS]; bool moved[NUM_PLAYERS]={false,false};
  for(int i=0;i<NUM_PLAYERS;i++){
    if(players[i].len==0||!players[i].alive) continue;
    players[i].dir=players[i].nextDir;
    Pt h=players[i].body[0];
    switch(players[i].dir){case R:h.x++;break;case L:h.x--;break;case U:h.y--;break;case D:h.y++;break;}
    newHead[i]=h; moved[i]=true;
  }

  bool anyDied=false;
  for(int i=0;i<NUM_PLAYERS;i++){
    if(!moved[i]) continue;
    Pt h=newHead[i];
    bool dead=(h.x<0||h.x>=GW||h.y<0||h.y>=GH);
    for(int k=0;k<players[i].len-1&&!dead;k++) dead=(players[i].body[k].x==h.x&&players[i].body[k].y==h.y);
    for(int j=0;j<NUM_PLAYERS&&!dead;j++){
      if(j==i) continue;
      for(int k=0;k<players[j].len&&!dead;k++) dead=(players[j].body[k].x==h.x&&players[j].body[k].y==h.y);
    }
    if(dead){ players[i].alive=false; anyDied=true; }
  }
  if(moved[0]&&moved[1]&&players[0].alive&&players[1].alive&&
     newHead[0].x==newHead[1].x&&newHead[0].y==newHead[1].y){
    players[0].alive=false; players[1].alive=false; anyDied=true;
  }
  if(anyDied){
    for(int i=0;i<NUM_PLAYERS;i++) if(players[i].score>hiScore){hiScore=players[i].score;prefs.putInt("hi",hiScore);}
    gameState=DEAD;
    flashLED(led.Color(50,0,0),600);broadcastGameOver();return;
  }

  bool anyAte=false;
  for(int i=0;i<NUM_PLAYERS;i++){
    if(!moved[i]) continue;
    Pt h=newHead[i];
    bool ate=(h.x==food.x&&h.y==food.y);
    int nl=players[i].len+(ate?1:0);
    for(int k=nl-1;k>0;k--) players[i].body[k]=players[i].body[k-1];
    players[i].body[0]=h; players[i].len=nl;
    if(ate){
      bool wasPower=foodPower;
      players[i].score+=wasPower?3:1;
      if(players[i].score%3==0&&tickMs>80)tickMs-=8;
      flashLED(wasPower?led.Color(50,40,0):led.Color(0,50,0),wasPower?150:80);
      anyAte=true;
    }
  }
  if(anyAte) spawnFood();
  broadcastState();
}

// ─── OLED ─────────────────────────────────────────────────────────────────────
void drawOLED(){
  oled.clearDisplay();
  if(gameState==IDLE){
    oled.setTextSize(2);oled.setTextColor(SSD1306_WHITE);oled.setCursor(22,4);oled.print("SNAKE");
    oled.setTextSize(1);oled.setCursor(0,28);oled.print("WiFi: "WIFI_SSID);
    oled.setCursor(0,40);oled.print("Open: ");oled.print(WiFi.localIP());
    oled.setCursor(0,54);oled.print("Players: ");oled.print((int)ws.count());
    oled.display();return;
  }
  if(gameState==DEAD){
    oled.setTextSize(2);oled.setTextColor(SSD1306_WHITE);oled.setCursor(16,4);oled.print("DEAD!");
    oled.setTextSize(1);
    if(players[1].connected||players[1].len>0){
      oled.setCursor(0,28);oled.print("P1: ");oled.print(players[0].score);
      oled.setCursor(0,40);oled.print("P2: ");oled.print(players[1].score);
      oled.setCursor(0,52);oled.print("Best: ");oled.print(hiScore);
    } else {
      oled.setCursor(0,30);oled.print("Score : ");oled.print(players[0].score);
      oled.setCursor(0,42);oled.print("Best  : ");oled.print(hiScore);
    }
    oled.display();return;
  }
  if(!foodPower || (millis()/200)%2==0){
    if(foodPower) oled.drawRect(food.x*CELL,food.y*CELL,CELL,CELL,SSD1306_WHITE);
    oled.fillRect(food.x*CELL+1,food.y*CELL+1,2,2,SSD1306_WHITE);
  }
  for(int i=0;i<NUM_PLAYERS;i++){
    for(int j=0;j<players[i].len;j++){
      int px=players[i].body[j].x*CELL,py=players[i].body[j].y*CELL;
      if(i==0){
        if(j==0) oled.fillRect(px,py,CELL,CELL,SSD1306_WHITE);
        else     oled.fillRect(px+1,py+1,CELL-2,CELL-2,SSD1306_WHITE);
      } else {
        oled.drawRect(px,py,CELL,CELL,SSD1306_WHITE);
      }
    }
  }
  oled.display();
}

// ─── WebSocket handler ────────────────────────────────────────────────────────
void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len){
  if(type==WS_EVT_CONNECT){
    int slot=assignSlot(client->id());
    Serial.printf("[ws] #%u connected as player %d\n",client->id(),slot);
    String you="{\"t\":\"you\",\"p\":"; you+=slot; you+="}";
    client->text(you);
    if(gameState==PLAYING) broadcastState(); else broadcastIdle();
    broadcastLeaderboard(); return;
  }
  if(type==WS_EVT_DISCONNECT){
    Serial.printf("[ws] #%u disconnected\n",client->id());
    int slot=playerSlotFor(client->id());
    if(slot>=0){
      players[slot].connected=false; players[slot].clientId=0;
      if(gameState==DEAD&&!submitted[slot]){
        submitted[slot]=true;
        bool allDone=true;
        for(int i=0;i<NUM_PLAYERS;i++) if(players[i].len>0&&!submitted[i]) allDone=false;
        if(allDone){ gameState=IDLE; broadcastIdle(); }
      }
    }
    return;
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
    int slot=playerSlotFor(client->id());
    if(slot>=0){
      Player&p=players[slot];
      const char* v=doc["v"]|"";
      if(strcmp(v,"R")==0&&p.dir!=L)p.nextDir=R;
      else if(strcmp(v,"L")==0&&p.dir!=R)p.nextDir=L;
      else if(strcmp(v,"U")==0&&p.dir!=D)p.nextDir=U;
      else if(strcmp(v,"D")==0&&p.dir!=U)p.nextDir=D;
    }
  }
  else if(strcmp(t,"start")==0){
    if(gameState==IDLE||gameState==DEAD){startGame();broadcastState();}
  }
  else if(strcmp(t,"submit")==0&&gameState==DEAD){
    int slot=playerSlotFor(client->id());
    if(slot>=0&&players[slot].len>0&&!submitted[slot]){
      const char* name=doc["n"]|"ANON";
      addToLeaderboard(name,players[slot].score);broadcastLeaderboard();
      submitted[slot]=true;
    }
    bool allDone=true;
    for(int i=0;i<NUM_PLAYERS;i++) if(players[i].len>0&&!submitted[i]) allDone=false;
    if(allDone){ gameState=IDLE; broadcastIdle(); }
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup(){
  Serial.begin(115200);
  led.begin();led.setBrightness(60);led.setPixelColor(0,led.Color(0,30,0));led.show();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASS);
  Serial.printf("[wifi] connecting to %s",WIFI_SSID);
  while(WiFi.status()!=WL_CONNECTED){ delay(400); Serial.print("."); }
  Serial.println();
  Serial.print("[wifi] connected, IP: "); Serial.println(WiFi.localIP());

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
  server.onNotFound([](AsyncWebServerRequest* req){ req->send(404,"text/plain","Not found"); });

  server.begin();
  Serial.println("[ready] http:80 ws:/ws");
  drawOLED();
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop(){
  ws.cleanupClients();
  updateLED();

  unsigned long now=millis();
  if(gameState==PLAYING){
    if(now-lastTick>=(unsigned long)tickMs){lastTick=now;gameTick();drawOLED();}
  } else {
    if(now-lastDraw>2000){lastDraw=now;drawOLED();}
  }
}
