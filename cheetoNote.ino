#include <SPI.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>

// Pins
#define TFT_DC    1
#define TFT_CS    3
#define TFT_RST   5
#define TFT_SCLK  6
#define TFT_MOSI  7
#define SD_MISO   2
#define SD_CS     10

#define BTN_DOWN     9   // pullup, LOW when pressed
#define BTN_UP   8   // pulldown, HIGH when pressed
#define BTN_SELECT 0   // pulldown, HIGH when pressed

// Colours
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define GREY    0x8410

// Display
#define SCREEN_W  128
#define SCREEN_H  160
#define CHAR_W    6
#define CHAR_H    8
#define MARGIN    2
#define CHARS_PER_LINE  ((SCREEN_W - MARGIN * 2) / CHAR_W)
#define LINES_PER_PAGE  ((SCREEN_H - MARGIN * 2) / CHAR_H)

Arduino_DataBus *bus = new Arduino_HWSPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, SD_MISO);
Arduino_GFX *gfx = new Arduino_ST7735(bus, TFT_RST, 0, false, 128, 160, 0, 0);

// ---- Button helpers ----

bool btnUp() {
    return digitalRead(BTN_UP) == HIGH;      // pulldown
}
bool btnDown() {
    return digitalRead(BTN_DOWN) == LOW;   // pullup
}
bool btnSelect() {
    return digitalRead(BTN_SELECT) == HIGH; // pulldown
}

void waitRelease() {
    delay(50);
    while (btnUp() || btnDown() || btnSelect()) delay(10);
    delay(50);
}

// ---- File browser ----
#define MAX_FILES 64
char fileList[MAX_FILES][32];  // store up to 64 filenames, 31 chars each
int fileCount = 0;
int selectedFile = 0;
int browserScroll = 0;         // top visible file index
#define BROWSER_LINES  ((SCREEN_H - CHAR_H - MARGIN * 2) / CHAR_H)  // lines minus header

void loadFileList() {
    fileCount = 0;
    File root = SD.open("/");
    while (fileCount < MAX_FILES) {
        File entry = root.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) {
            // Only show .txt files
            const char* name = entry.name();
            int len = strlen(name);
            if (len > 4 && strcmp(name + len - 4, ".txt") == 0) {
                strncpy(fileList[fileCount], name, 31);
                fileList[fileCount][31] = '\0';
                fileCount++;
            }
        }
        entry.close();
    }
    root.close();
}

void drawBrowser() {
    gfx->fillScreen(BLACK);

    // Header
    gfx->setTextColor(BLACK);
    gfx->fillRect(0, 0, SCREEN_W, CHAR_H + MARGIN, WHITE);
    gfx->setCursor(MARGIN, MARGIN);
    gfx->setTextSize(1);
    gfx->print("Select a file");

    if (fileCount == 0) {
        gfx->setTextColor(RED);
        gfx->setCursor(MARGIN, CHAR_H + MARGIN * 2);
        gfx->print("No .txt files found");
        return;
    }

    // File list
    for (int i = 0; i < BROWSER_LINES; i++) {
        int fileIdx = browserScroll + i;
        if (fileIdx >= fileCount) break;

        int y = CHAR_H + MARGIN + i * CHAR_H;
        bool isSelected = (fileIdx == selectedFile);

        if (isSelected) {
            gfx->fillRect(0, y, SCREEN_W, CHAR_H, BLUE);
            gfx->setTextColor(WHITE);
        } else {
            gfx->setTextColor(GREY);
        }

        gfx->setCursor(MARGIN, y);

        // Truncate filename if too long
        char display[CHARS_PER_LINE + 1];
        strncpy(display, fileList[fileIdx], CHARS_PER_LINE);
        display[CHARS_PER_LINE] = '\0';
        gfx->print(display);
    }

    // Scroll indicator
    if (fileCount > BROWSER_LINES) {
        int barH = max(4, BROWSER_LINES * BROWSER_LINES * CHAR_H / (fileCount * CHAR_H));
        int barY = CHAR_H + MARGIN + (browserScroll * (SCREEN_H - CHAR_H - MARGIN - barH) / (fileCount - BROWSER_LINES));
        gfx->fillRect(SCREEN_W - 2, CHAR_H + MARGIN, 2, SCREEN_H - CHAR_H - MARGIN, BLACK);
        gfx->fillRect(SCREEN_W - 2, barY, 2, barH, GREY);
    }
}

void browserUp() {
    if (selectedFile > 0) {
        selectedFile--;
        if (selectedFile < browserScroll) browserScroll = selectedFile;
        drawBrowser();
    }
}

void browserDown() {
    if (selectedFile < fileCount - 1) {
        selectedFile++;
        if (selectedFile >= browserScroll + BROWSER_LINES) browserScroll = selectedFile - BROWSER_LINES + 1;
        drawBrowser();
    }
}

// ---- Text reader ----
#define MAX_PAGE_HISTORY 64
long pageHistory[MAX_PAGE_HISTORY];
int historyDepth = 0;
char currentFile[40];

long displayPage(const char* filename, long filePos) {
    File f = SD.open(filename);
    if (!f) {
        gfx->fillScreen(BLACK);
        gfx->setTextColor(RED);
        gfx->setCursor(MARGIN, MARGIN);
        gfx->println("File not found!");
        return -1;
    }

    f.seek(filePos);
    gfx->fillScreen(BLACK);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(1);

    int line = 0;
    int col = 0;

    while (f.available() && line < LINES_PER_PAGE) {
        char c = f.read();
        if (c == '\r') continue;
        if (c == '\n') { line++; col = 0; continue; }
        if (col >= CHARS_PER_LINE) { line++; col = 0; }
        if (line < LINES_PER_PAGE) {
            gfx->drawChar(MARGIN + col * CHAR_W, MARGIN + line * CHAR_H, c, WHITE, BLACK);
            col++;
        }
    }

    long newPos = f.position();
    bool eof = !f.available();
    f.close();
    return eof ? -1 : newPos;
}

void openFile(const char* filename) {
    snprintf(currentFile, sizeof(currentFile), "/%s", filename);
    historyDepth = 0;
    pageHistory[historyDepth] = 0;
    displayPage(currentFile, 0);
}

// ---- App state ----
enum AppState { BROWSING, READING };
AppState state = BROWSING;

void setup() {
    Serial0.begin(115200);
    delay(500);

    pinMode(BTN_UP,     INPUT_PULLDOWN);
    pinMode(BTN_DOWN,   INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLDOWN);

    SPI.begin(TFT_SCLK, SD_MISO, TFT_MOSI);

    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    delay(100);

    if (!SD.begin(SD_CS, SPI, 4000000)) {
        Serial0.println("SD init failed!");
        while (1);
    }

    if (!gfx->begin()) {
        Serial0.println("Display init failed!");
        while (1);
    }

    loadFileList();
    drawBrowser();
}

void loop() {
    if (state == BROWSING) {
        if (btnUp()) {
            waitRelease();
            browserUp();
        }
        if (btnDown()) {
            waitRelease();
            browserDown();
        }
        if (btnSelect() && fileCount > 0) {
            waitRelease();
            state = READING;
            openFile(fileList[selectedFile]);
        }
    }

    else if (state == READING) {
        long currentPos = pageHistory[historyDepth];

        // Next page
        if (btnDown()) {
            waitRelease();
            long nextPos = displayPage(currentFile, currentPos);
            if (nextPos != -1) {
                if (historyDepth < MAX_PAGE_HISTORY - 1) {
                    historyDepth++;
                    pageHistory[historyDepth] = nextPos;
                }
                displayPage(currentFile, pageHistory[historyDepth]);
            }
        }

        // Previous page
        if (btnUp()) {
            waitRelease();
            if (historyDepth > 0) {
                historyDepth--;
                displayPage(currentFile, pageHistory[historyDepth]);
            }
        }

        // Back to browser
        if (btnSelect()) {
            waitRelease();
            state = BROWSING;
            drawBrowser();
        }
    }
}