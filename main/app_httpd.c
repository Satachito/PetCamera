#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"

#include "app_httpd.h"
#include "app_camera.h"
#include "app_motion.h"
#include "app_audio.h"
#include "app_orientation.h"
#include "app_ota.h"
#include "app_setup.h"
#include "app_wifi.h"
#include "frame_bus.h"

static const char *TAG = "httpd";

#define BOUNDARY "tab5petcamframe"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" BOUNDARY;
static const char *STREAM_BOUNDARY     = "\r\n--" BOUNDARY "\r\n";
static const char *STREAM_PART_HEADER  = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

/* If no new frame arrives in this long the camera task is wedged; end the
 * response so the browser can retry rather than hang on a dead connection. */
#define FRAME_WAIT_TIMEOUT_MS 5000

/* Read size when streaming a recorded clip off the card. */
#define CLIP_CHUNK_BYTES 8192

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

static atomic_int s_clients;

/* The stream lives on a different port, so the page builds its URL from the
 * host the browser used — works for both an IP and the .local name. */
static const char VIEWER_PAGE[] =
    "<!doctype html><html lang='ja'><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Tab5 Pet Camera</title><style>"
    ":root{color-scheme:dark}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:#0e0f12;color:#e6e7ea;"
    "font:15px/1.5 -apple-system,BlinkMacSystemFont,'Hiragino Sans',sans-serif}"
    "header{display:flex;align-items:center;gap:.6rem;padding:.9rem 1rem;"
    "border-bottom:1px solid #24262c}"
    "h1{font-size:1rem;font-weight:600;margin:0}"
    "#dot{width:9px;height:9px;border-radius:50%;background:#3ddc84;flex:none}"
    "#dot.stale{background:#e5484d}"
    "main{padding:1rem;max-width:960px;margin:0 auto}"
    "#stage{display:flex;justify-content:center}"
    /* The frame keeps the rotated image clipped and centred; JS sets its exact
       size because the aspect ratio flips with the rotation. */
    "#wrap{position:relative;background:#000;border-radius:10px;overflow:hidden}"
    "#view{position:absolute;top:50%;left:50%;transform-origin:50% 50%;"
    "transform:translate(-50%,-50%);transition:transform .2s ease}"
    "#stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(90px,1fr));"
    "gap:.5rem;margin-top:1rem}"
    ".s{background:#171920;border:1px solid #24262c;border-radius:8px;padding:.6rem .7rem}"
    ".s b{display:block;font-size:1.05rem;font-weight:600}"
    ".s span{font-size:.72rem;color:#8b8e98;text-transform:uppercase;letter-spacing:.04em}"
    "a.btn,button.btn{display:inline-block;margin-top:1rem;padding:.55rem 1rem;"
    "background:#171920;border:1px solid #24262c;border-radius:8px;color:#e6e7ea;"
    "text-decoration:none;font:inherit;cursor:pointer}"
    "h2{font-size:.8rem;font-weight:600;color:#8b8e98;text-transform:uppercase;"
    "letter-spacing:.05em;margin:1.6rem 0 .6rem}"
    ".clip{display:flex;align-items:center;gap:.75rem;padding:.6rem .75rem;"
    "background:#171920;border:1px solid #24262c;border-radius:8px;margin-bottom:.4rem}"
    ".clip b{flex:1;font-weight:500;font-size:.9rem}"
    ".clip .sz{color:#8b8e98;font-size:.8rem;white-space:nowrap}"
    ".clip button,.clip a{background:none;border:0;color:#3ddc84;font:inherit;"
    "cursor:pointer;text-decoration:none;padding:0 .25rem}"
    "#clips,#sounds{color:#8b8e98;font-size:.85rem}"
    "#bar2{display:flex;gap:.75rem;flex-wrap:wrap;align-items:center}"
    ".hint{color:#8b8e98;font-size:.8rem;margin-top:1rem;align-self:center}"
    "#vol{display:flex;align-items:center;gap:.5rem;margin-top:1rem;"
    "font-size:.8rem;color:#8b8e98}"
    "#volr{accent-color:#3ddc84}"
    "button.btn.on{background:#14361f;border-color:#2b6b41;color:#3ddc84}"
    "button.mini{background:none;border:0;color:#e5484d;font:inherit;font-size:.72rem;"
    "text-transform:none;letter-spacing:0;cursor:pointer;margin-left:.5rem}"
    ".clip .del{color:#e5484d}"
    "</style></head><body>"
    "<header><i id='dot'></i><h1>Tab5 Pet Camera</h1></header>"
    "<main>"
    "<div id='stage'><div id='wrap'><img id='view' alt='live view'></div></div>"
    "<div id='stats'>"
    "<div class='s'><b id='fps'>-</b><span>fps</span></div>"
    "<div class='s'><b id='res'>-</b><span>size</span></div>"
    "<div class='s'><b id='kb'>-</b><span>kb/frame</span></div>"
    "<div class='s'><b id='rot'>-</b><span>rotation</span></div>"
    "<div class='s'><b id='mode'>-</b><span>transport</span></div>"
    "</div>"
    "<div id='bar'>"
    "<a class='btn' href='/snapshot' download='snapshot.jpg'>Save a snapshot</a>"
    "<button class='btn' id='live' style='display:none'>Back to live</button>"
    "</div>"
    "<h2>Sound</h2>"
    "<div id='bar2'>"
    "<button class='btn' id='listen'>Listen</button>"
    "<span class='hint'>Press the microphone button on the Tab5 to record. Rename here afterwards.</span>"
    "<label id='vol'>Volume <input type='range' id='volr' min='0' max='100' value='100'></label>"
    "</div>"
    "<div id='sounds'>none yet</div>"
    "<h2>Recorded clips <button class='mini' id='delall'>Delete all</button></h2>"
    "<div id='clips'>none yet</div>"
    "</main><script>"
    "var wrap=document.getElementById('wrap');"
    "var img=document.getElementById('view');"
    "var stage=document.getElementById('stage');"
    "var rot=0,frames=0,mode='stream';"
    /* Each poll is a fresh connection. At 80 ms the sockets left in TIME_WAIT
       outran the lwIP pool and the server stopped accepting entirely. */
    "var POLL_MS=250;"
    /* A quarter turn swaps which source axis maps to the screen's width, so the
       frame and the image swap dimensions together and the result still fills. */
    "function layout(){"
    "var quarter=(rot%180)!==0;"
    "var ar=quarter?9/16:16/9;"
    "var availW=stage.clientWidth||360;"
    "var maxH=window.innerHeight*0.7;"
    "var w=availW,h=w/ar;"
    "if(h>maxH){h=maxH;w=h*ar;}"
    "wrap.style.width=w+'px';wrap.style.height=h+'px';"
    "img.style.width=(quarter?h:w)+'px';"
    "img.style.height=(quarter?w:h)+'px';"
    "img.style.transform='translate(-50%,-50%) rotate('+rot+'deg)';"
    "}"
    "function setRotation(deg){if(deg===rot)return;rot=deg;layout();}"
    /* The clip list sits well below the viewer, so bring the picture back into
       view rather than starting playback off-screen.
       scrollIntoView and the options form of scrollTo are both no-ops in some
       embedded browsers — verified here — while the two-argument scrollTo works
       everywhere. Try the nice one, then fall back by checking whether the page
       actually moved. */
    "function scrollToPlayer(){"
    "var y=stage.getBoundingClientRect().top+window.scrollY"
    "-Math.max(0,(window.innerHeight-stage.offsetHeight)/2);"
    "y=Math.max(0,Math.round(y));"
    "var from=window.scrollY;"
    "try{window.scrollTo({top:y,behavior:'smooth'});}catch(e){}"
    "setTimeout(function(){"
    "if(Math.abs(window.scrollY-from)<2&&Math.abs(from-y)>4){window.scrollTo(0,y);}"
    "},350);"
    "}"
    "window.addEventListener('resize',layout);layout();"
    /* MJPEG is one long-lived connection and much the cheaper option, but it has
       to live on its own port because a never-ending response would block the
       single-threaded server that serves this page. That makes it cross-origin,
       and some browsers and extensions refuse it. Falling back to same-origin
       snapshot polling keeps the camera usable wherever that happens. */
    "function useStream(){"
    "mode='stream';"
    "img.src='http://'+location.hostname+':' + STREAM_PORT + '/stream';"
    "}"
    /* Load the next snapshot into a detached Image first and only swap it in
       once it has decoded. Assigning straight to the visible element blanks it
       for the whole download, which at ~100 KB a frame is most of the time. */
    "function poll(){"
    "var next=new Image();"
    "next.onload=function(){img.src=next.src;frames++;setTimeout(poll,POLL_MS);};"
    "next.onerror=function(){setTimeout(poll,1000);};"
    "next.src='/snapshot?t='+Date.now();"
    "}"
    "function usePolling(){"
    "if(mode==='poll')return;"
    "mode='poll';"
    "img.onload=null;img.onerror=null;"
    "document.getElementById('mode').textContent='poll';"
    "poll();"
    "}"
    "img.onload=function(){frames++;};"
    "img.onerror=function(){if(mode==='stream'){usePolling();}};"
    "useStream();"
    /* onerror does not fire for every kind of block, so give the stream a few
       seconds to produce anything at all before giving up on it. */
    "setTimeout(function(){if(mode==='stream'&&frames===0){usePolling();}},4000);"
    /* Playback splits the .mjpeg client-side. Streaming the file and cutting it
       at each end-of-image marker means the page stays same-origin, nothing has
       to buffer a whole clip, and the server is never held open by a second
       endless response the way the live stream holds port 81. */
    "var playing=false,objUrl=null;"
    "var liveBtn=document.getElementById('live');"
    "function backToLive(){"
    "playing=false;liveBtn.style.display='none';"
    "if(objUrl){URL.revokeObjectURL(objUrl);objUrl=null;}"
    "if(mode==='poll'){poll();}else{useStream();}"
    "}"
    "liveBtn.onclick=backToLive;"
    "function showFrame(bytes){"
    "var next=URL.createObjectURL(new Blob([bytes],{type:'image/jpeg'}));"
    "img.src=next;"
    "if(objUrl){URL.revokeObjectURL(objUrl);}"
    "objUrl=next;"
    "}"
    "async function play(name){"
    "playing=true;liveBtn.style.display='inline-block';"
    "scrollToPlayer();"
    "img.onload=null;img.onerror=null;img.src='';"
    "var r=await fetch('/clip?name='+encodeURIComponent(name));"
    "var rd=r.body.getReader();var buf=new Uint8Array(0);"
    "var delay=1000/" TOSTRING(CONFIG_PETCAM_CLIP_FPS) ";"
    "while(playing){"
    "var c=await rd.read();"
    "if(c.done)break;"
    "var merged=new Uint8Array(buf.length+c.value.length);"
    "merged.set(buf);merged.set(c.value,buf.length);buf=merged;"
    "var i=0;"
    "while(i<buf.length-1){"
    "if(buf[i]===0xFF&&buf[i+1]===0xD9){"
    "showFrame(buf.subarray(0,i+2));"
    "buf=buf.slice(i+2);i=0;"
    "await new Promise(function(r){setTimeout(r,delay);});"
    "if(!playing)break;"
    "}else{i++;}"
    "}"
    "}"
    "try{rd.cancel();}catch(e){}"
    "if(playing){backToLive();}"
    "}"
    "function refreshClips(){"
    "fetch('/clips').then(function(r){return r.json();}).then(function(d){"
    "var box=document.getElementById('clips');"
    "if(!d.mounted){box.textContent='no SD card';return;}"
    /* readdir returns entries in the order FAT happens to hold them, which is
       not chronological. Order by modification time, newest first. */
    "var list=d.clips.filter(function(c){return c.bytes>0;});"
    "list.sort(function(a,b){return b.mtime-a.mtime;});"
    "if(!list.length){box.textContent='none yet';return;}"
    "box.innerHTML='';"
    "list.forEach(function(c){"
    "var row=document.createElement('div');row.className='clip';"
    "var nm=document.createElement('b');"
    /* mtime covers both naming schemes: clips recorded before NTP set the clock
       are named by uptime, and their timestamps are meaningless either way. */
    "nm.textContent=c.mtime>1000000000?new Date(c.mtime*1000).toLocaleString():"
    "c.name.replace('clip_','').replace('.mjpeg','');"
    "var sz=document.createElement('span');sz.className='sz';"
    "sz.textContent=(c.bytes/1048576).toFixed(1)+' MB';"
    "var pb=document.createElement('button');pb.textContent='Play';"
    "pb.onclick=function(){play(c.name);};"
    "var dl=document.createElement('a');dl.textContent='Download';"
    "dl.href='/clip?name='+encodeURIComponent(c.name);dl.download=c.name;"
    "var rm=document.createElement('button');rm.textContent='Delete';rm.className='del';"
    "rm.onclick=function(){"
    "if(!confirm('Delete this clip?'))return;"
    "fetch('/clipdelete?name='+encodeURIComponent(c.name)).then(refreshClips);"
    "};"
    "row.appendChild(nm);row.appendChild(sz);row.appendChild(pb);row.appendChild(dl);"
    "row.appendChild(rm);"
    "box.appendChild(row);"
    "});"
    "}).catch(function(){});"
    "}"
    "document.getElementById('delall').onclick=function(){"
    "if(!confirm('Delete every recorded clip? This cannot be undone.'))return;"
    "fetch('/clipdelete?all=1').then(refreshClips);"
    "};"
    "refreshClips();setInterval(refreshClips,15000);"
    /* Listening pulls half a second of WAV at a time and schedules each piece to
       start exactly where the previous one ended, so the pieces play as one
       continuous sound. Web Audio needs a user gesture to start, which the
       Listen button provides. */
    "var ac=null,listening=false,nextAt=0;"
    "var listenBtn=document.getElementById('listen');"
    "async function listenLoop(){"
    "while(listening){"
    "try{"
    "var r=await fetch('/audio');"
    "if(!r.ok){await new Promise(function(k){setTimeout(k,500);});continue;}"
    "var buf=await ac.decodeAudioData(await r.arrayBuffer());"
    "var src=ac.createBufferSource();"
    "src.buffer=buf;src.connect(ac.destination);"
    /* A little lead-in absorbs jitter; without it the first late chunk is
       scheduled in the past and is dropped, which sounds like a gap. */
    "if(nextAt<ac.currentTime+0.05){nextAt=ac.currentTime+0.2;}"
    "src.start(nextAt);nextAt+=buf.duration;"
    /* Stay close to live rather than building a backlog. */
    "while(listening&&nextAt-ac.currentTime>1.5){"
    "await new Promise(function(k){setTimeout(k,100);});"
    "}"
    "}catch(e){await new Promise(function(k){setTimeout(k,500);});}"
    "}"
    "}"
    "listenBtn.onclick=function(){"
    "if(listening){listening=false;listenBtn.textContent='Listen';listenBtn.className='btn';return;}"
    "if(!ac){ac=new (window.AudioContext||window.webkitAudioContext)();}"
    "ac.resume();nextAt=0;listening=true;"
    "listenBtn.textContent='Stop listening';listenBtn.className='btn on';"
    "listenLoop();"
    "};"
    "function refreshSounds(){"
    "fetch('/sounds').then(function(r){return r.json();}).then(function(list){"
    "var box=document.getElementById('sounds');"
    "if(!list.length){box.textContent='none yet';return;}"
    "box.innerHTML='';"
    "list.forEach(function(n){"
    "var row=document.createElement('div');row.className='clip';"
    "var nm=document.createElement('b');nm.textContent=n.replace('.wav','');"
    "var pb=document.createElement('button');pb.textContent='Play on Tab5';"
    "pb.onclick=function(){fetch('/playsound?name='+encodeURIComponent(n));};"
    "var dl=document.createElement('a');dl.textContent='Download';"
    "dl.href='/sound?name='+encodeURIComponent(n);dl.download=n;"
    "var rn=document.createElement('button');rn.textContent='Rename';"
    "rn.onclick=function(){"
    "var to=prompt('New name',n.replace('.wav',''));"
    "if(!to)return;"
    "fetch('/renamesound?from='+encodeURIComponent(n)+'&to='+encodeURIComponent(to))"
    ".then(function(r){return r.ok?r.json():r.text().then(function(t){throw new Error(t);});})"
    ".then(function(d){"
    "if(d.name&&d.name!==to+'.wav'){alert('Saved as '+d.name+'\\n\\nOnly a-z, 0-9, - and _ can be used.');}"
    "refreshSounds();"
    "}).catch(function(e){alert('Could not rename: that name may already be taken.');});"
    "};"
    "var rm=document.createElement('button');rm.textContent='Delete';rm.className='del';"
    "rm.onclick=function(){"
    "if(!confirm('Delete '+n+'?'))return;"
    "fetch('/deletesound?name='+encodeURIComponent(n)).then(refreshSounds);"
    "};"
    "row.appendChild(nm);row.appendChild(pb);row.appendChild(dl);"
    "row.appendChild(rn);row.appendChild(rm);"
    "box.appendChild(row);"
    "});"
    "}).catch(function(){});"
    "}"
    "document.getElementById('volr').onchange=function(){"
    "fetch('/volume?percent='+this.value);"
    "};"
    "refreshSounds();setInterval(refreshSounds,15000);"

    "setInterval(function(){fetch('/status').then(function(r){return r.json();}).then(function(d){"
    "document.getElementById('fps').textContent=d.fps.toFixed(1);"
    "document.getElementById('res').textContent=d.width+'x'+d.height;"
    "document.getElementById('kb').textContent=(d.jpeg_bytes/1024).toFixed(0);"
    "document.getElementById('rot').textContent=d.view_rotation+'\\u00b0';"
    "if(mode==='stream'){document.getElementById('mode').textContent='mjpeg';}"
    "document.getElementById('dot').className=d.fps>0.5?'':'stale';"
    "setRotation(d.view_rotation);"
    "}).catch(function(){document.getElementById('dot').className='stale';});},2000);"
    "</script></body></html>";

/* httpd_query_key_value hands back the raw percent-encoded value. Without this
 * a name typed in Japanese becomes a filename literally spelled
 * "%E3%83%9E%E3%83%AC%E3%82%A2.wav", and every later request re-encodes the
 * percent signs so nothing ever matches it again — which is why Delete had no
 * effect on those files. */
static void url_decode(char *s)
{
    char *out = s;

    while (*s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *out++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *out++ = ' ';
            s++;
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

static esp_err_t page_handler(httpd_req_t *req)
{
    /* STREAM_PORT is substituted here rather than baked into the string so the
     * Kconfig port stays the single source of truth. */
    static char *page;

    if (!page) {
        const char *marker = "STREAM_PORT";
        size_t len = sizeof(VIEWER_PAGE) + 32;
        page = malloc(len);
        if (!page) {
            return httpd_resp_send_500(req);
        }
        char port[8];
        snprintf(port, sizeof(port), "%d", CONFIG_PETCAM_STREAM_PORT);

        const char *src = VIEWER_PAGE;
        char *dst = page;
        const char *hit;
        while ((hit = strstr(src, marker)) != NULL) {
            memcpy(dst, src, hit - src);
            dst += hit - src;
            dst += sprintf(dst, "%s", port);
            src = hit + strlen(marker);
        }
        strcpy(dst, src);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
    uint32_t seq = 0;
    frame_t *frame = frame_bus_acquire_latest(&seq, FRAME_WAIT_TIMEOUT_MS);

    if (!frame) {
        /* esp_http_server has no 503 in its error enum, so set the line by hand. */
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "no frame available yet");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
    esp_err_t err = httpd_resp_send(req, (const char *)frame->data, frame->len);
    frame_bus_release(frame);
    return err;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    app_camera_stats_t stats;
    app_motion_stats_t motion = { 0 };
    float ax = 0, ay = 0, az = 0;
    int quadrant = -1, rotation = 0;
    int ppa_deg, view_deg;
    int audio_ms_left = 0;
    int audio_last_bytes = 0;
    const char *audio_last_result = "none";
    char json[640];

    app_camera_get_stats(&stats);
#if CONFIG_PETCAM_ENABLE_MOTION
    app_motion_get_stats(&motion);
#endif
#if CONFIG_PETCAM_AUTO_ROTATE
    app_orientation_get_accel(&ax, &ay, &az, &quadrant);
    rotation = app_orientation_get();
#endif

    /* What a remote viewer must do to see the image the same way up as the Tab5
     * shows it. LVGL turns the preview and the on-screen UI by the same amount,
     * so relative to the upright UI the image is rotated by exactly the PPA
     * angle — counter-clockwise. CSS rotate() goes clockwise, hence 360 minus. */
    /* Fetched before the snprintf: C does not define the order arguments are
     * evaluated in, so reading the byte count in the same call that fills it is
     * not guaranteed to see the new value. */
    audio_last_result = app_audio_last_record(&audio_last_bytes);

    ppa_deg = ((CONFIG_PETCAM_CAMERA_MOUNT_ROTATION + rotation) % 360 + 360) % 360;
    view_deg = (360 - ppa_deg) % 360;
    snprintf(json, sizeof(json),
             "{\"fps\":%.1f,\"width\":%u,\"height\":%u,\"jpeg_bytes\":%u,"
             "\"frames\":%u,\"dropped\":%u,\"clients\":%d,\"rssi\":%d,"
             "\"uptime_s\":%llu,\"free_heap\":%u,\"free_psram\":%u,"
             "\"motion\":{\"armed\":%s,\"recording\":%s,\"score\":%u,"
             "\"events\":%u,\"clips\":%u,\"deleted\":%u,\"free_mb\":%u},"
             "\"orientation\":{\"rotation\":%d,\"raw\":%d,"
             "\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f},"
             "\"view_rotation\":%d,\"preview_rotation\":%d,"
             "\"saved_networks\":%d,"
             "\"audio\":{\"recording\":%s,\"playing\":%s,\"phase\":%d,\"ms_left\":%d,"
             "\"last_bytes\":%d,\"last_result\":\"%s\"},"
             "\"partition\":\"%s\"}",
             stats.fps, (unsigned)stats.width, (unsigned)stats.height,
             (unsigned)stats.last_jpeg_len, (unsigned)stats.frames,
             (unsigned)stats.dropped, atomic_load(&s_clients), app_wifi_rssi(),
             (unsigned long long)(esp_timer_get_time() / 1000000),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             motion.armed ? "true" : "false",
             motion.recording ? "true" : "false",
             (unsigned)motion.last_score, (unsigned)motion.events,
             (unsigned)motion.clips_written, (unsigned)motion.clips_deleted,
             (unsigned)(motion.free_bytes / (1024 * 1024)),
             rotation, quadrant, ax, ay, az, view_deg, ppa_deg,
             app_wifi_saved_count(),
             app_audio_is_recording() ? "true" : "false",
             app_audio_is_playing() ? "true" : "false",
             (int)app_audio_get_phase(&audio_ms_left), audio_ms_left,
             audio_last_bytes, audio_last_result,
             app_ota_running_partition());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* Recorded clips are only useful if they can be reviewed without pulling the
 * card out of the device. */
/* Lists both the clips directory and the card root, so recordings made before
 * the move to a subdirectory stay visible and deletable. */
static bool list_clips_in(httpd_req_t *req, const char *directory, bool first)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    char line[360];

    if (!dir) {
        return first;
    }
    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        char path[300];

        if (!strstr(entry->d_name, ".mjpeg")) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"bytes\":%ld,\"mtime\":%lld}",
                 first ? "" : ",", entry->d_name, (long)st.st_size,
                 (long long)st.st_mtime);
        httpd_resp_sendstr_chunk(req, line);
        first = false;
    }
    closedir(dir);
    return first;
}

static esp_err_t clips_handler(httpd_req_t *req)
{
    bool first = true;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"mounted\":true,\"clips\":[");
    first = list_clips_in(req, CONFIG_BSP_SD_MOUNT_POINT "/clips", first);
    list_clips_in(req, CONFIG_BSP_SD_MOUNT_POINT, first);
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t clip_handler(httpd_req_t *req)
{
    char query[160];
    char name[96];
    char path[300];
    FILE *f;
    char *buf;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name is required");
        return ESP_FAIL;
    }
    url_decode(name);

    /* The name goes straight into a path, so anything that could climb out of
     * the mount point is refused rather than sanitised. */
    if (strchr(name, '/') || strstr(name, "..") || !strstr(name, ".mjpeg")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }

    snprintf(path, sizeof(path), "%s/clips/%s", CONFIG_BSP_SD_MOUNT_POINT, name);
    f = fopen(path, "rb");
    if (!f) {
        /* Might predate the move into a subdirectory. */
        snprintf(path, sizeof(path), "%s/%s", CONFIG_BSP_SD_MOUNT_POINT, name);
        f = fopen(path, "rb");
    }
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such clip");
        return ESP_FAIL;
    }

    buf = malloc(CLIP_CHUNK_BYTES);
    if (!buf) {
        fclose(f);
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "video/x-motion-jpeg");
    for (;;) {
        size_t n = fread(buf, 1, CLIP_CHUNK_BYTES, f);
        if (n == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

#if CONFIG_PETCAM_ENABLE_MOTION
static esp_err_t clipdelete_handler(httpd_req_t *req)
{
    char query[160];
    char name[96];
    char reply[64];
    int removed;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "name", name, sizeof(name)) == ESP_OK) {
        url_decode(name);
        if (strchr(name, '/') || strstr(name, "..") || !strstr(name, ".mjpeg")) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
            return ESP_FAIL;
        }
        removed = app_motion_delete(name);
    } else if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
               httpd_query_key_value(query, "all", name, sizeof(name)) == ESP_OK) {
        removed = app_motion_delete(NULL);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name= or all=1 required");
        return ESP_FAIL;
    }

    snprintf(reply, sizeof(reply), "{\"deleted\":%d}", removed);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, reply, HTTPD_RESP_USE_STRLEN);
}

/* Diagnostic: where the detector thinks the change is. */
static esp_err_t motionmap_handler(httpd_req_t *req)
{
    uint8_t cells[1024];
    int w = 0, h = 0;
    int n = app_motion_get_diffmap(cells, sizeof(cells), &w, &h);
    char head[64];

    httpd_resp_set_type(req, "application/json");
    snprintf(head, sizeof(head), "{\"w\":%d,\"h\":%d,\"delta\":[", w, h);
    httpd_resp_sendstr_chunk(req, head);
    for (int i = 0; i < n; i++) {
        char v[8];
        snprintf(v, sizeof(v), "%s%u", i ? "," : "", cells[i]);
        httpd_resp_sendstr_chunk(req, v);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}
#endif

static esp_err_t setupstate_handler(httpd_req_t *req)
{
    char hint[64] = "";
    bool open = false;
    int rows = 0;
    char json[160];

    app_setup_get_state(&open, hint, sizeof(hint), &rows);
    snprintf(json, sizeof(json), "{\"open\":%s,\"hint\":\"%s\",\"rows\":%d}",
             open ? "true" : "false", hint, rows);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t setupopen_handler(httpd_req_t *req)
{
    app_setup_open();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "setup panel opened\n");
}

/* Isolates "does scanning work" from "does the on-screen panel work". */
static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_ap_record_t *records = calloc(20, sizeof(*records));
    uint16_t found = 0;
    int64_t started = esp_timer_get_time();
    esp_err_t err;
    char line[128];

    if (!records) {
        return httpd_resp_send_500(req);
    }

    err = app_wifi_scan(records, 20, &found);

    httpd_resp_set_type(req, "application/json");
    snprintf(line, sizeof(line), "{\"err\":\"%s\",\"ms\":%lld,\"found\":%u,\"aps\":[",
             esp_err_to_name(err), (long long)((esp_timer_get_time() - started) / 1000),
             (unsigned)found);
    httpd_resp_sendstr_chunk(req, line);
    for (int i = 0; i < found; i++) {
        snprintf(line, sizeof(line), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 i ? "," : "", (const char *)records[i].ssid, records[i].rssi);
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    free(records);
    return ESP_OK;
}

/* Half a second of WAV per request. Short chunks keep this on the same origin
 * and off a dedicated server instance; see app_audio.h for why that matters. */
#define AUDIO_CHUNK_MS 500

static esp_err_t audio_handler(httpd_req_t *req)
{
    const size_t bytes = (size_t)APP_AUDIO_SAMPLE_RATE * (APP_AUDIO_BITS / 8) *
                         APP_AUDIO_CHANNELS * AUDIO_CHUNK_MS / 1000;
    uint8_t header[44];
    uint8_t *pcm;
    size_t got;

    pcm = malloc(bytes);
    if (!pcm) {
        return httpd_resp_send_500(req);
    }

    got = app_audio_read(pcm, bytes, 2000);
    if (got == 0) {
        free(pcm);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "no audio");
        return ESP_FAIL;
    }

    {
        const uint32_t rate = APP_AUDIO_SAMPLE_RATE;
        const uint16_t ch = APP_AUDIO_CHANNELS;
        const uint16_t bits = APP_AUDIO_BITS;

        memcpy(header, "RIFF", 4);
        *(uint32_t *)(header + 4)  = 36 + got;
        memcpy(header + 8, "WAVEfmt ", 8);
        *(uint32_t *)(header + 16) = 16;
        *(uint16_t *)(header + 20) = 1;
        *(uint16_t *)(header + 22) = ch;
        *(uint32_t *)(header + 24) = rate;
        *(uint32_t *)(header + 28) = rate * ch * bits / 8;
        *(uint16_t *)(header + 32) = ch * bits / 8;
        *(uint16_t *)(header + 34) = bits;
        memcpy(header + 36, "data", 4);
        *(uint32_t *)(header + 40) = got;
    }

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    httpd_resp_send_chunk(req, (const char *)pcm, got);
    httpd_resp_send_chunk(req, NULL, 0);
    free(pcm);
    return ESP_OK;
}

static esp_err_t volume_handler(httpd_req_t *req)
{
    char query[64];
    char value[8];
    char json[48];
    int percent = -1;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "percent", value, sizeof(value)) == ESP_OK) {
        percent = atoi(value);
        app_audio_set_volume(percent);
    }

    snprintf(json, sizeof(json), "{\"volume\":%d}", percent);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t sounds_handler(httpd_req_t *req)
{
    DIR *dir = opendir(CONFIG_BSP_SD_MOUNT_POINT "/sounds");
    struct dirent *entry;
    bool first = true;

    httpd_resp_set_type(req, "application/json");
    if (!dir) {
        return httpd_resp_sendstr(req, "[]");
    }
    httpd_resp_sendstr_chunk(req, "[");
    while ((entry = readdir(dir)) != NULL) {
        char line[300];

        if (!strstr(entry->d_name, ".wav")) {
            continue;
        }
        snprintf(line, sizeof(line), "%s\"%s\"", first ? "" : ",", entry->d_name);
        httpd_resp_sendstr_chunk(req, line);
        first = false;
    }
    closedir(dir);
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static bool safe_sound_name(const char *name)
{
    return name[0] && !strchr(name, '/') && !strstr(name, "..") && strstr(name, ".wav");
}

/* Downloading a recording: /clip only serves .mjpeg, deliberately. */
static esp_err_t sound_handler(httpd_req_t *req)
{
    char query[160];
    char name[96];
    char path[300];
    char *buf;
    FILE *f;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        (url_decode(name), !safe_sound_name(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name=<file>.wav required");
        return ESP_FAIL;
    }

    snprintf(path, sizeof(path), "%s/sounds/%s", CONFIG_BSP_SD_MOUNT_POINT, name);
    f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such sound");
        return ESP_FAIL;
    }

    buf = malloc(CLIP_CHUNK_BYTES);
    if (!buf) {
        fclose(f);
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "audio/wav");
    for (;;) {
        size_t n = fread(buf, 1, CLIP_CHUNK_BYTES, f);

        if (n == 0 || httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t renamesound_handler(httpd_req_t *req)
{
    char query[256];
    char from[96];
    char to[96];
    char from_path[300];
    char to_path[300];
    char reply[160];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "from", from, sizeof(from)) != ESP_OK ||
        httpd_query_key_value(query, "to", to, sizeof(to)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "from= and to= required");
        return ESP_FAIL;
    }
    url_decode(from);
    url_decode(to);

    /* The new name is typed on a phone, so it arrives with whatever the keyboard
     * offered. Reduce it to what FAT can hold before it becomes a path. */
    app_audio_normalise_name(to, sizeof(to));
    if (!strstr(to, ".wav")) {
        strlcat(to, ".wav", sizeof(to));
    }
    if (!safe_sound_name(from) || !safe_sound_name(to)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unusable name");
        return ESP_FAIL;
    }

    snprintf(from_path, sizeof(from_path), "%s/sounds/%s", CONFIG_BSP_SD_MOUNT_POINT, from);
    snprintf(to_path, sizeof(to_path), "%s/sounds/%s", CONFIG_BSP_SD_MOUNT_POINT, to);

    /* Refuse rather than silently replace: two sounds with the same purpose are
     * easy to create by accident and impossible to get back. */
    if (access(to_path, F_OK) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "that name is taken");
        return ESP_FAIL;
    }

    snprintf(reply, sizeof(reply), "{\"renamed\":%s,\"name\":\"%s\"}",
             rename(from_path, to_path) == 0 ? "true" : "false", to);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, reply);
}

static esp_err_t deletesound_handler(httpd_req_t *req)
{
    char query[160];
    char name[96];
    char path[300];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        (url_decode(name), !safe_sound_name(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name=<file>.wav required");
        return ESP_FAIL;
    }

    snprintf(path, sizeof(path), "%s/sounds/%s", CONFIG_BSP_SD_MOUNT_POINT, name);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, unlink(path) == 0 ? "{\"deleted\":true}"
                                                     : "{\"deleted\":false}");
}

static esp_err_t playsound_handler(httpd_req_t *req)
{
    char query[160];
    char name[96];
    esp_err_t err;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        (url_decode(name), !safe_sound_name(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name=<file>.wav required");
        return ESP_FAIL;
    }

    err = app_audio_play(name);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, err == ESP_OK ? "{\"queued\":true}"
                                                 : "{\"queued\":false}");
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    uint32_t last_seq = 0;
    esp_err_t err;

    err = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (err != ESP_OK) {
        return err;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* MJPEG is a single response that never ends; a keep-alive socket returned
     * to the pool afterwards would be in an undefined state. */
    httpd_resp_set_hdr(req, "Connection", "close");

    atomic_fetch_add(&s_clients, 1);
    ESP_LOGI(TAG, "viewer connected (%d total)", atomic_load(&s_clients));

    for (;;) {
        char part[64];
        frame_t *frame = frame_bus_acquire_latest(&last_seq, FRAME_WAIT_TIMEOUT_MS);

        if (!frame) {
            ESP_LOGW(TAG, "no frame for %d ms, closing the stream", FRAME_WAIT_TIMEOUT_MS);
            break;
        }

        int header_len = snprintf(part, sizeof(part), STREAM_PART_HEADER, (unsigned)frame->len);

        err = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, part, header_len);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)frame->data, frame->len);
        }
        frame_bus_release(frame);

        if (err != ESP_OK) {
            /* Normal path when the viewer closes the tab. */
            ESP_LOGI(TAG, "viewer went away");
            break;
        }
    }

    atomic_fetch_sub(&s_clients, 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t app_httpd_start(void)
{
    httpd_handle_t page_server = NULL;
    httpd_handle_t stream_server = NULL;

    httpd_config_t page_cfg = HTTPD_DEFAULT_CONFIG();
    page_cfg.server_port      = CONFIG_PETCAM_HTTP_PORT;
    page_cfg.ctrl_port        = 32768;
    page_cfg.max_open_sockets = 7;
    /* Default is 8 and this server is past that. Overflowing it used to abort at
     * boot, which turned "one endpoint too many" into a boot loop. */
    page_cfg.max_uri_handlers  = 24;
    /* Handlers touch FATFS; the 4 KB default leaves very little headroom. */
    page_cfg.stack_size        = 8192;
    page_cfg.lru_purge_enable = true;

    httpd_config_t stream_cfg = HTTPD_DEFAULT_CONFIG();
    stream_cfg.server_port      = CONFIG_PETCAM_STREAM_PORT;
    stream_cfg.ctrl_port        = 32769;
    stream_cfg.max_open_sockets = 3;
    stream_cfg.max_uri_handlers = 4;
    stream_cfg.lru_purge_enable = true;
    /* Sending a full frame over a slow link can take a while; the default 5 s
     * send timeout would abort otherwise. */
    stream_cfg.send_wait_timeout = 15;
    stream_cfg.stack_size        = 6144;

    static const httpd_uri_t page_uri     = { .uri = "/",         .method = HTTP_GET, .handler = page_handler };
    static const httpd_uri_t snapshot_uri = { .uri = "/snapshot", .method = HTTP_GET, .handler = snapshot_handler };
    static const httpd_uri_t status_uri   = { .uri = "/status",   .method = HTTP_GET, .handler = status_handler };
    static const httpd_uri_t clips_uri    = { .uri = "/clips",    .method = HTTP_GET, .handler = clips_handler };
    static const httpd_uri_t clip_uri     = { .uri = "/clip",     .method = HTTP_GET, .handler = clip_handler };
    static const httpd_uri_t stream_uri   = { .uri = "/stream",   .method = HTTP_GET, .handler = stream_handler };

    ESP_RETURN_ON_ERROR(httpd_start(&page_server, &page_cfg), TAG, "page server failed to start");
/* One endpoint too many should cost that endpoint, not abort the boot. */
#define REGISTER(server, u) do {                                              \
        esp_err_t _e = httpd_register_uri_handler((server), &(u));            \
        if (_e != ESP_OK) {                                                   \
            ESP_LOGE(TAG, "cannot register %s: %s", (u).uri,                  \
                     esp_err_to_name(_e));                                    \
        }                                                                     \
    } while (0)
    REGISTER(page_server, page_uri);
    REGISTER(page_server, snapshot_uri);
    REGISTER(page_server, status_uri);
    REGISTER(page_server, clips_uri);
    REGISTER(page_server, clip_uri);
    static const httpd_uri_t scan_uri  = { .uri = "/scan",      .method = HTTP_GET, .handler = scan_handler };
    static const httpd_uri_t setup_uri  = { .uri = "/setupopen",  .method = HTTP_GET, .handler = setupopen_handler };
    static const httpd_uri_t sstate_uri = { .uri = "/setupstate", .method = HTTP_GET, .handler = setupstate_handler };
    REGISTER(page_server, scan_uri);
    REGISTER(page_server, setup_uri);
    REGISTER(page_server, sstate_uri);
    static const httpd_uri_t audio_uri  = { .uri = "/audio",       .method = HTTP_GET, .handler = audio_handler };
    static const httpd_uri_t sounds_uri = { .uri = "/sounds",      .method = HTTP_GET, .handler = sounds_handler };
    static const httpd_uri_t plays_uri  = { .uri = "/playsound",   .method = HTTP_GET, .handler = playsound_handler };
    REGISTER(page_server, audio_uri);
    REGISTER(page_server, sounds_uri);
    REGISTER(page_server, plays_uri);
    static const httpd_uri_t sound_uri = { .uri = "/sound", .method = HTTP_GET, .handler = sound_handler };
    static const httpd_uri_t dels_uri = { .uri = "/deletesound", .method = HTTP_GET, .handler = deletesound_handler };
    REGISTER(page_server, sound_uri);
    static const httpd_uri_t ren_uri = { .uri = "/renamesound", .method = HTTP_GET, .handler = renamesound_handler };
    REGISTER(page_server, dels_uri);
    REGISTER(page_server, ren_uri);
    static const httpd_uri_t vol_uri = { .uri = "/volume", .method = HTTP_GET, .handler = volume_handler };
    REGISTER(page_server, vol_uri);
    if (app_ota_register(page_server) != ESP_OK) {
        ESP_LOGE(TAG, "cannot register /update; OTA is unavailable");
    }
#if CONFIG_PETCAM_ENABLE_MOTION
    static const httpd_uri_t motionmap_uri  = { .uri = "/motionmap",  .method = HTTP_GET, .handler = motionmap_handler };
    static const httpd_uri_t clipdelete_uri = { .uri = "/clipdelete", .method = HTTP_GET, .handler = clipdelete_handler };
    REGISTER(page_server, motionmap_uri);
    REGISTER(page_server, clipdelete_uri);
#endif

    ESP_RETURN_ON_ERROR(httpd_start(&stream_server, &stream_cfg), TAG, "stream server failed to start");
    REGISTER(stream_server, stream_uri);

    ESP_LOGI(TAG, "page on :%d, stream on :%d", CONFIG_PETCAM_HTTP_PORT, CONFIG_PETCAM_STREAM_PORT);
    return ESP_OK;
}

int app_httpd_client_count(void)
{
    return atomic_load(&s_clients);
}
