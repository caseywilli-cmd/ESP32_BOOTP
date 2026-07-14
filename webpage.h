#pragma once
// ==========================================================================
// Dashboard HTML/CSS/JS, split out of the main .ino on purpose
// ==========================================================================
// This file holds the exact same PROGMEM string that used to sit inline in
// the .ino. It was moved here because Arduino's sketch preprocessor (the
// ctags-based scanner that auto-generates function prototypes) does not
// understand C++11 raw string literals -- it scans their contents as if
// they were ordinary C++ code. Every JavaScript "function name() {" inside
// the dashboard's <script> block matched its "return-type name(args) {"
// heuristic, so it tried to generate a prototype like "function loadData();"
// for each one -- which fails to compile, since "function" isn't a C++ type.
// ("error: 'function' does not name a type")
//
// That scanner only runs on the sketch's main .ino file(s), not on any .h or
// .cpp files pulled in via #include. Moving the raw string here sidesteps
// the bug entirely -- nothing else changes; index_html is used exactly the
// same way from esp_bootp_v0_14.ino via send_P().
//
// Keep this file in the SAME sketch folder as the .ino -- the Arduino IDE
// will show it as an extra tab automatically.

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>Rockwell Commissioning Tool</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    /* ---- Design tokens -------------------------------------------------
       Centralizing colors/spacing here means the whole dashboard's look
       can be tweaked from one place instead of hunting through rules. */
    :root {
      --bg: #f3f4f6;
      --surface: #ffffff;
      --border: #e2e5ea;
      --text: #1f2430;
      --text-muted: #6b7280;
      --primary: #2563eb;
      --primary-hover: #1d4ed8;
      --success: #16a34a;
      --success-bg: #ecfdf3;
      --danger: #dc2626;
      --danger-hover: #b91c1c;
      --radius: 10px;
      --shadow: 0 1px 2px rgba(15,23,42,.06), 0 1px 3px rgba(15,23,42,.08);
    }

    * { box-sizing: border-box; }

    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      margin: 0;
      background: var(--bg);
      color: var(--text);
    }

    /* Dark top bar so the tool reads as an instrument, not a random web page */
    .topbar {
      background: #111827;
      color: #f9fafb;
      padding: 16px 24px;
      display: flex;
      align-items: baseline;
      gap: 10px;
      flex-wrap: wrap;
    }
    .topbar h1 { margin: 0; font-size: 1.05rem; font-weight: 600; letter-spacing: .01em; }
    .topbar .tag { color: #9ca3af; font-size: .8rem; }

    .container { max-width: 960px; margin: 0 auto; padding: 24px; }

    .card {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      box-shadow: var(--shadow);
      padding: 20px 24px;
      margin-bottom: 20px;
    }
    .card h3 {
      margin: 0 0 16px 0;
      padding-bottom: 12px;
      border-bottom: 1px solid var(--border);
      font-size: .8rem;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: .05em;
      color: var(--text-muted);
    }

    .field-row { display: flex; gap: 16px; flex-wrap: wrap; align-items: flex-end; }
    .field { display: flex; flex-direction: column; gap: 6px; }
    .field label { font-size: .75rem; font-weight: 600; color: var(--text-muted); }

    .current-ip {
      display: inline-block;
      font-family: "SFMono-Regular", Consolas, monospace;
      font-size: .95rem;
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 6px;
      padding: 6px 10px;
      margin-bottom: 16px;
    }

    input[type=text] {
      padding: 8px 10px;
      width: 150px;
      border: 1px solid var(--border);
      border-radius: 6px;
      font-size: .9rem;
      background: var(--surface);
      color: var(--text);
    }
    input[type=text]:focus {
      outline: none;
      border-color: var(--primary);
      box-shadow: 0 0 0 3px rgba(37,99,235,.15);
    }

    button {
      background-color: var(--primary);
      color: white;
      border: none;
      padding: 9px 14px;
      cursor: pointer;
      border-radius: 6px;
      font-size: .85rem;
      font-weight: 600;
      transition: background-color .15s ease;
    }
    button:hover { background-color: var(--primary-hover); }
    button:disabled { background-color: #c3c9d1; cursor: not-allowed; }
    button.danger { background-color: var(--danger); margin-left: 8px; }
    button.danger:hover { background-color: var(--danger-hover); }
    button.small { padding: 6px 10px; font-size: .8rem; }

    .table-wrap { overflow-x: auto; } /* keeps the table usable on a tablet */
    table { width: 100%; border-collapse: collapse; font-size: .87rem; }
    thead th {
      text-align: left;
      padding: 10px 12px;
      background: #f9fafb;
      color: var(--text-muted);
      font-size: .72rem;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: .04em;
      border-bottom: 2px solid var(--border);
      white-space: nowrap;
    }
    tbody td { padding: 10px 12px; border-bottom: 1px solid var(--border); vertical-align: middle; }
    tbody tr:hover { background: #f9fafb; }
    tbody tr:last-child td { border-bottom: none; }

    .mac-cell { font-family: "SFMono-Regular", Consolas, monospace; font-size: .85rem; }
    .vendor-tag { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; font-size: .74rem; color: var(--text-muted); margin-top: 2px; }

    .status-dot { display: inline-block; width: 9px; height: 9px; border-radius: 50%; margin-right: 6px; }
    .status-fresh { background-color: var(--success); }
    .status-stale { background-color: #b0b0b0; }
    .age { color: var(--text-muted); font-size: .82rem; }

    .ip-pill {
      display: inline-block;
      padding: 3px 9px;
      border-radius: 999px;
      font-family: "SFMono-Regular", Consolas, monospace;
      font-size: .82rem;
    }
    .ip-pill.assigned { background: var(--success-bg); color: var(--success); }
    .ip-pill.unassigned { background: var(--bg); color: var(--text-muted); }

    .assign-cell { display: flex; gap: 6px; align-items: center; }
    .assign-cell input[type=text] { width: 130px; }

    .ping-cell { display: flex; gap: 8px; align-items: center; }
    .ping-result { font-size: .82rem; }
    .ping-ok { color: var(--success); font-weight: 600; }
    .ping-fail { color: var(--danger); font-weight: 600; }

    .empty-row td { text-align: center; color: var(--text-muted); padding: 24px 12px; font-size: .88rem; }

    .footer-note { text-align: center; color: var(--text-muted); font-size: .75rem; margin-top: 8px; }
  </style>
</head>
<body>
  <div class="topbar">
    <h1>Network Commissioning Tool</h1>
    <span class="tag">BOOTP / DHCP static IP assignment</span>
  </div>

  <div class="container">
    <div class="card">
      <h3>Tool Network Settings</h3>
      <div class="current-ip">Current IP: <span id="tool_ip">Loading...</span></div>
      <div class="field-row">
        <div class="field">
          <label for="new_ip">Static IP</label>
          <input type="text" id="new_ip" placeholder="192.168.1.100">
        </div>
        <div class="field">
          <label for="new_sn">Subnet Mask</label>
          <input type="text" id="new_sn" placeholder="255.255.255.0">
        </div>
        <div class="field">
          <button onclick="updateSettings()">Save &amp; Reboot</button>
        </div>
        <div class="field">
          <button class="danger" onclick="rebootTool()">Reboot Now</button>
        </div>
      </div>
    </div>

    <div class="card">
      <h3>Discovered Devices (BOOTP/DHCP)</h3>
      <div class="table-wrap">
        <table id="deviceTable">
          <thead>
            <tr><th>MAC Address</th><th>Status</th><th>Target IP</th><th>Assign</th><th>Ping</th></tr>
          </thead>
          <tbody id="deviceTableBody"></tbody>
        </table>
      </div>
    </div>

    <div class="footer-note">Firmware v0.14</div>
  </div>

  <script>
    // ---- Vendor identification (MAC OUI lookup) ---------------------------
    // The first 3 bytes of a MAC address (the OUI) are assigned by the IEEE
    // to a specific manufacturer, so they can be used to guess what a
    // discovered device actually is before it has a name or IP. This table
    // is scoped to Rockwell Automation / Allen-Bradley prefixes (verified
    // against the IEEE MA-L registry) since that's what this tool exists to
    // commission -- most devices a technician sees here should tag as
    // "Rockwell Automation" (which covers Allen-Bradley PLCs, drives, I/O,
    // and HMIs, since Allen-Bradley is a Rockwell brand and shares its OUI
    // block). Devices whose OUI isn't in the table just show no vendor tag
    // rather than a guess.
    //
    // To identify other equipment you commonly see on-site (managed
    // switches, VFDs from another vendor, etc.), add more "OUI: vendor
    // name" entries below -- no firmware rebuild needed, this is plain JS
    // served from flash. Look up additional OUIs at https://maclookup.app/.
    const OUI_VENDORS = {
      '0000BC': 'Rockwell Automation',
      '001D9C': 'Rockwell Automation',
      '086195': 'Rockwell Automation',
      '184C08': 'Rockwell Automation',
      '34C0F9': 'Rockwell Automation',
      '404101': 'Rockwell Automation',
      '44CC6E': 'Rockwell Automation',
      '5C2167': 'Rockwell Automation',
      '5C8816': 'Rockwell Automation',
      '68C8EB': 'Rockwell Automation',
      'BCF499': 'Rockwell Automation',
      'E48EBB': 'Rockwell Automation',
      'E49069': 'Rockwell Automation',
      'F45433': 'Rockwell Automation',
    };

    // Returns a vendor name for a "AA:BB:CC:DD:EE:FF" MAC string, or null
    // if the OUI isn't in our table.
    function lookupVendor(mac) {
      let oui = mac.replace(/:/g, '').substring(0, 6).toUpperCase();
      return OUI_VENDORS[oui] || null;
    }

    // ---- Device table -----------------------------------------------------
    // Polled every 2s (see setInterval near the bottom). Existing rows are
    // updated in place rather than re-rendered so an in-progress ping result
    // or a focused input field isn't clobbered on every refresh.
    function loadData() {
      fetch('/api/devices')
        .then(r => r.json())
        .then(data => {
          let tbody = document.getElementById('deviceTableBody');

          // Show a friendly placeholder instead of a bare empty table while
          // waiting for the first device to show up on the field network.
          if (data.length === 0) {
            tbody.innerHTML = '<tr class="empty-row"><td colspan="5">No devices discovered yet. Waiting for a BOOTP/DHCP request on the field network&hellip;</td></tr>';
            return;
          }
          let placeholder = tbody.querySelector('.empty-row');
          if (placeholder) tbody.innerHTML = '';

          data.forEach(d => {
            let existingRow = document.getElementById('row_' + d.mac);
            let fresh = d.age < 30; // "fresh" = heard from in the last 30s

            if (existingRow) {
              // Row already exists -- just patch the bits that can change.
              existingRow.querySelector('.age').innerText = d.age + "s ago";
              existingRow.querySelector('.status-dot').className = 'status-dot ' + (fresh ? 'status-fresh' : 'status-stale');
              let ipCell = existingRow.querySelector('.targetIp');
              ipCell.innerHTML = d.targetIp
                ? `<span class="ip-pill assigned">${d.targetIp}</span>`
                : `<span class="ip-pill unassigned">Unassigned</span>`;
              let pingBtn = document.getElementById('pingBtn_' + d.mac);
              if (pingBtn) pingBtn.disabled = !d.targetIp;
            } else {
              // First time seeing this MAC -- build a new row for it. The
              // vendor tag is looked up once here since the MAC (and
              // therefore its OUI) never changes for a given row.
              let row = document.createElement('tr');
              row.id = 'row_' + d.mac;
              let vendor = lookupVendor(d.mac);
              row.innerHTML = `
                <td class="mac-cell">${d.mac}${vendor ? `<div class="vendor-tag">${vendor}</div>` : ''}</td>
                <td><span class="status-dot ${fresh ? 'status-fresh' : 'status-stale'}"></span><span class="age">${d.age}s ago</span></td>
                <td class="targetIp">${d.targetIp ? `<span class="ip-pill assigned">${d.targetIp}</span>` : `<span class="ip-pill unassigned">Unassigned</span>`}</td>
                <td>
                  <div class="assign-cell">
                    <input type='text' id='ip_${d.mac}' placeholder='192.168.1.xxx' value='${d.targetIp || ""}'>
                    <button class="small" onclick='assignIP("${d.mac}")'>Assign</button>
                  </div>
                </td>
                <td>
                  <div class="ping-cell">
                    <button class="small" id='pingBtn_${d.mac}' onclick='pingDevice("${d.mac}")' ${d.targetIp ? "" : "disabled"}>Ping</button>
                    <span id='ping_${d.mac}' class='ping-result'></span>
                  </div>
                </td>`;
              tbody.appendChild(row);
            }
          });
        });
    }

    // Sends the technician-entered IP for one MAC to the tool. The tool
    // just remembers it (pendingReplyType gets set on the C++ side) and
    // answers with it the next time that device sends a BOOTP/DHCP request
    // -- it does not push the change to the device proactively.
    function assignIP(mac) {
      let ip = document.getElementById('ip_' + mac).value;
      if(!ip) { alert("Enter an IP first!"); return; }
      fetch(`/api/assign?mac=${mac}&ip=${ip}`).then(r => r.text()).then(alert);
    }

    function rebootTool() {
      if (!confirm('Reboot the ESP32 now? The web UI will be unavailable for a few seconds.')) return;
      fetch('/api/reboot').then(r => r.text()).then(alert);
    }

    // Saves the TOOL's own IP/subnet (not a device's) and reboots so the
    // new Ethernet config takes effect via ETH.config() in setup().
    function updateSettings() {
      let ip = document.getElementById('new_ip').value;
      let sn = document.getElementById('new_sn').value;
      fetch(`/api/set_network?ip=${ip}&sn=${sn}`).then(r => r.text()).then(msg => {
          alert(msg);
          setTimeout(() => location.reload(), 3000);
      });
    }

    // Fires an ICMP ping (over the wired Ethernet interface) at an assigned
    // device to confirm the new static IP actually took and is reachable.
    function pingDevice(mac) {
      let ip = document.getElementById('ip_' + mac).value;
      if(!ip) { alert("No IP assigned yet!"); return; }
      let btn = document.getElementById('pingBtn_' + mac);
      let result = document.getElementById('ping_' + mac);
      btn.disabled = true;
      result.className = 'ping-result';
      result.innerText = 'Pinging...';
      fetch(`/api/ping?ip=${ip}`)
        .then(r => r.json())
        .then(data => {
          btn.disabled = false;
          if (data.error) {
            result.className = 'ping-result ping-fail';
            result.innerText = data.error;
            return;
          }

          if (data.received > 0) {
            result.className = 'ping-result ping-ok';
            // The underlying ESP-IDF ping library reports round-trip time
            // truncated to whole milliseconds, so genuinely fast replies on
            // a direct local Ethernet link often come back as 0ms. Floor
            // the displayed value at 1ms so it doesn't read as "no time
            // elapsed" -- the device is still alive and responding.
            let displayMs = data.timeMs > 0 ? data.timeMs : 1;
            result.innerText = `${data.received}/${data.transmitted} replies, ${displayMs}ms`;
          } else {
            result.className = 'ping-result ping-fail';
            result.innerText = `No reply (0/${data.transmitted})`;
          }

        })
        .catch(() => {
          btn.disabled = false;
          result.className = 'ping-result ping-fail';
          result.innerText = 'Ping request failed';
        });
    }

    // Reflects the tool's current own IP/subnet in the top card. Skips
    // overwriting a field the technician is actively typing into so a
    // background poll doesn't yank the cursor out from under them.
    function loadSettings() {
      fetch('/api/settings').then(r => r.json()).then(data => {
        document.getElementById('tool_ip').innerText = data.ip + " / " + data.sn;
        let ipField = document.getElementById('new_ip');
        let snField = document.getElementById('new_sn');
        // Don't stomp on a field the technician is actively editing.
        if (document.activeElement !== ipField) ipField.value = data.ip;
        if (document.activeElement !== snField) snField.value = data.sn;
      });
    }

    // Poll both the device list and the tool's own settings every 2s.
    setInterval(() => { loadData(); loadSettings(); }, 2000);
    window.onload = () => { loadData(); loadSettings(); };
  </script>
</body>
</html>
)rawliteral";
