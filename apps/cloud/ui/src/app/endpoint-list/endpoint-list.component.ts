import { Component, OnInit, OnDestroy, Output, EventEmitter } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';
import { DebugService } from '../../common/debug.service';

interface EpInfo { endpoint:string; tun_ip:string; dev_tun_ip?:string; isp_ip?:string; lan_ip?:string;
                   proxy_port:number; registered:boolean;
                   last_seen_unix?:number; lifetime?:number; location?:string; }

/** Per-endpoint credential record, as minted by iot-cloudd into the
 *  cloud.endpoint.credentials ds key (JSON array). Field names follow the
 *  dotted convention used server-side (see cloud_credentials.cpp). */
interface EpCred {
  serial?: string;
  identity?: string;
  'bs.psk.key'?: string;
  'dm.psk.id'?: string;
  'dm.psk.key'?: string;
}

@Component({
  selector: 'app-endpoint-list',
  template: `
    <div class="page">
      <div class="header-row">
        <h3>Endpoints</h3>
        <span></span>
      </div>

      <!-- PSK provisioning (task O): paste serial + device-generated BS PSK
           from device-ui. Backend forms rpi<serial>@cloud.local, mints the
           DM PSK, and stores per-endpoint credentials. -->
      <div class="card" *ngIf="isAdmin" style="margin-bottom:20px;">
        <div class="card-header">{{ editing ? 'Update Bootstrap PSK — ' + provSerial : 'Provision a device' }}</div>
        <div class="card-block">
          <div class="form-grid">
            <clr-input-container>
              <label>Serial Number</label>
              <input clrInput [(ngModel)]="provSerial" [readonly]="editing" style="width:100%;"
                     placeholder="device serial (from device-ui)" />
              <clr-control-helper *dsDebug><app-ds-hint key="cloud.provision.request"></app-ds-hint></clr-control-helper>
            </clr-input-container>
            <clr-input-container>
              <label>Bootstrap PSK (32 hex)</label>
              <input clrInput [(ngModel)]="provBsPsk" style="width:100%;font-family:monospace;"
                     placeholder="paste BS PSK from device-ui" />
              <clr-control-helper *dsDebug><app-ds-hint key="cloud.provision.bs.psk"></app-ds-hint></clr-control-helper>
            </clr-input-container>
            <div></div>
            <div></div>
          </div>
          <button class="btn btn-primary" style="margin-top:16px;"
                  [disabled]="provisioning" (click)="provision()">
            {{ provisioning ? (editing ? 'Updating…' : 'Provisioning…') : (editing ? 'Update PSK' : 'Provision') }}
          </button>
          <button *ngIf="editing" class="btn btn-link" style="margin-top:16px;"
                  [disabled]="provisioning" (click)="cancelEdit()">Cancel</button>
          <span *ngIf="editing" class="hint" style="margin-left:12px;">
            Re-provisions <code>{{ provSerial }}</code> in place (keeps tunnel IP / proxy port) —
            paste the device's newly-generated BS PSK.
          </span>
          <span *ngIf="!editing && !devMode" class="hint" style="margin-left:12px;">
            Note: enable <code>cloud.dev.mode</code> to store credentials during commissioning.
          </span>
        </div>
      </div>

      <clr-datagrid>
        <clr-dg-column>Endpoint</clr-dg-column>
        <clr-dg-column>State</clr-dg-column>
        <clr-dg-column>Tunnel IP</clr-dg-column>
        <clr-dg-column>Device Tunnel IP</clr-dg-column>
        <clr-dg-column>ISP IP</clr-dg-column>
        <clr-dg-column>LAN IP</clr-dg-column>
        <clr-dg-column>Proxy Port</clr-dg-column>
        <clr-dg-column>Next Heart Beat in</clr-dg-column>
        <clr-dg-column>Location</clr-dg-column>
        <clr-dg-column>Device UI</clr-dg-column>
        <clr-dg-column *ngIf="isAdmin">Actions</clr-dg-column>

        <clr-dg-row *clrDgItems="let e of endpoints">
          <clr-dg-cell>
            <a style="cursor:pointer" title="Show on map" (click)="showOnMap.emit(e.endpoint)"><code>{{e.endpoint}}</code></a>
          </clr-dg-cell>
          <clr-dg-cell>
            <app-status-badge [label]="e.registered?'online':'offline'"
              [state]="e.registered?'connected':'exited'"></app-status-badge>
          </clr-dg-cell>
          <clr-dg-cell><code>{{ serverTunIp || '—' }}</code></clr-dg-cell>
          <clr-dg-cell><code>{{ e.dev_tun_ip || '—' }}</code></clr-dg-cell>
          <clr-dg-cell><code>{{ e.isp_ip || '—' }}</code></clr-dg-cell>
          <clr-dg-cell><code>{{ e.lan_ip || '—' }}</code></clr-dg-cell>
          <clr-dg-cell>{{e.proxy_port}}</clr-dg-cell>
          <clr-dg-cell>
            <code>{{ nextHeartbeat(e) }}</code>
            <!-- Debug mode: reveal the raw ds values the countdown derives from
                 (cloud.endpoints[].lifetime + .last_seen_unix), mirroring the
                 *dsDebug hint convention used on the config forms. -->
            <span *ngIf="debug.on" class="hint">
              (lt {{ e.lifetime || 0 }}s · seen {{ e.last_seen_unix || 0 }})
            </span>
          </clr-dg-cell>
          <clr-dg-cell>
            <code>{{ e.location || '—' }}</code>
          </clr-dg-cell>
          <clr-dg-cell>
            <!-- Launch UI only when the device's VPN tunnel is up (dev_tun_ip).
                 Same-origin path-scoped reverse proxy: iot-httpd proxies
                 /dev/<ep>/ over the tun to the device UI (one HTTPS origin,
                 per-device cookie isolation). Requires iot-httpd to share
                 iot-cloudd's netns — see apps/docs/tdd-device-ui-path-proxy.md. -->
            <!-- Device-UI reachability follows the VPN tunnel only (dev_tun_ip +
                 proxy), NOT LwM2M registration: the reverse proxy works whenever
                 the tunnel is up, even if the device's LwM2M registration has
                 lapsed. Registration status is shown separately in the State
                 column. -->
            <a class="btn btn-sm" target="_blank" rel="noopener"
               [href]="launchUrl(e.endpoint)"
               *ngIf="e.dev_tun_ip">
              Launch UI <clr-icon shape="pop-out" size="12"></clr-icon>
            </a>
            <span *ngIf="!e.dev_tun_ip" class="hint">VPN down</span>
          </clr-dg-cell>
          <clr-dg-cell *ngIf="isAdmin">
            <button class="btn btn-sm btn-outline" (click)="editPsk(e.endpoint)">Edit</button>
            <button class="btn btn-sm btn-danger" (click)="deprovision(e.endpoint)">Remove</button>
          </clr-dg-cell>
        </clr-dg-row>

        <!-- Read-only per-endpoint credentials minted into
             cloud.endpoint.credentials by the provisioning flow. PSK secrets
             are admin-gated; non-admins see a placeholder. -->
        <clr-dg-detail *clrIfDetail="let e">
          <clr-dg-detail-header>Provisioned Credentials — {{e.endpoint}}</clr-dg-detail-header>
          <clr-dg-detail-body>
            <!-- Device UI over VPN: the per-device nftables DNAT installed by
                 iot-cloudd (cloud:<proxy_port> → <tun_ip>:<ui_port> over tun0). -->
            <dl class="cred-list" style="margin-bottom:16px;">
              <dt>VPN forwarding rule</dt>
              <dd>
                <code>tcp dport {{ e.proxy_port }} &rarr; {{ e.dev_tun_ip || e.tun_ip }}:{{ uiPort }}</code>
                <span class="hint" style="margin-left:8px;" *ngIf="e.dev_tun_ip">DNAT — device UI over VPN</span>
                <span class="hint" style="margin-left:8px;" *ngIf="!e.dev_tun_ip">VPN down — rule inactive</span>
              </dd>
            </dl>
            <ng-container *ngIf="credFor(e) as c; else noCred">
              <dl class="cred-list">
                <dt>Serial</dt><dd><code>{{ c.serial || '—' }}</code></dd>
                <dt>DM PSK Identity</dt><dd><code>{{ c.identity || c['dm.psk.id'] || '—' }}</code></dd>
                <dt>BS PSK Key</dt><dd><code>{{ secret(c['bs.psk.key']) }}</code></dd>
                <dt>DM PSK Identity (key id)</dt><dd><code>{{ c['dm.psk.id'] || '—' }}</code></dd>
                <dt>DM PSK Key</dt><dd><code>{{ secret(c['dm.psk.key']) }}</code></dd>
              </dl>
              <p *ngIf="!isAdmin" class="hint">Sign in as an admin to reveal PSK secrets.</p>
            </ng-container>
            <ng-template #noCred>
              <p class="hint">No provisioned credentials found for this endpoint.</p>
            </ng-template>
          </clr-dg-detail-body>
        </clr-dg-detail>

        <clr-dg-footer>{{endpoints.length}} endpoint{{endpoints.length===1?'':'s'}}</clr-dg-footer>
      </clr-datagrid>

      <p *ngIf="endpoints.length===0" style="color:#888;margin-top:16px;">
        No devices provisioned yet. Go to <strong>Provision</strong> to add one.
      </p>
    </div>
  `,
  styles: [`
    .page{padding:24px;} h3{color:#333;margin:0;font-size:16px;font-weight:600;}
    .header-row{display:flex;align-items:center;justify-content:space-between;margin:0 0 16px 0;}
    .hint{font-size:12px;color:#888;}
    .cred-list{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;margin:0;}
    .cred-list dt{color:#666;font-weight:600;}
    .cred-list dd{margin:0;word-break:break-all;}
  `]
})
export class EndpointListComponent implements OnInit, OnDestroy {
  /// Emits the endpoint serial when its name is clicked → MainComponent shows
  /// it on the Fleet Map.
  @Output() showOnMap = new EventEmitter<string>();
  endpoints: EpInfo[] = [];
  creds: EpCred[] = [];
  // Live wall-clock (unix s), ticked every second so the "Next Heart Beat in"
  // countdown re-renders without re-fetching the endpoint list.
  now = Math.floor(Date.now() / 1000);
  private ticker?: ReturnType<typeof setInterval>;
  windowHost = window.location.hostname;
  // Device UI port reached over the VPN (cloud.proxy.device.ui.port,
  // default 80). Shown in the per-endpoint "VPN forwarding rule" line.
  uiPort = 80;
  // VPN server's tunnel IP (first host of cloud.vpn.subnet, e.g. 10.9.0.1) —
  // the server end of every tunnel. Shown in the "Tunnel IP" column.
  serverTunIp = '';
  // PSK provisioning (task O).
  provSerial = ''; provBsPsk = ''; provisioning = false; devMode = false;
  // Edit mode: re-provision an existing endpoint to update its BS PSK in place.
  editing = false;
  private sub = new Subscription(); private active = true;

  get isAdmin(): boolean { return this.session.isAdmin; }

  /** Same-origin path-scoped device-UI URL (reverse-proxied by iot-httpd over
   *  the VPN tun). The endpoint is URL-encoded so urn:dev:* names are path-safe.
   *  See apps/docs/tdd-device-ui-path-proxy.md. */
  launchUrl(ep: string): string { return '/dev/' + encodeURIComponent(ep) + '/'; }

  /** First host of a CIDR subnet ("10.9.0.0/24" → "10.9.0.1") = the OpenVPN
   *  server's tun IP under `topology subnet`. */
  private firstHost(subnet: string): string {
    const base = (subnet || '').split('/')[0];
    const o = base.split('.');
    if (o.length !== 4) return '';
    o[3] = String((Number(o[3]) || 0) + 1);
    return o.join('.');
  }

  constructor(private http: HttpsvcService, private session: SessionService,
              private toast: ToastService, public debug: DebugService) {}

  /** Format a non-negative second count as HH:MM:SS (hours uncapped). */
  private hhmmss(total: number): string {
    const s = Math.max(0, Math.floor(total));
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = s % 60;
    const p = (n: number) => String(n).padStart(2, '0');
    return `${p(h)}:${p(m)}:${p(sec)}`;
  }

  /** Time until the device's next expected registration refresh, HH:MM:SS:
   *  (last_seen + lifetime) - now. '—' when we have no lifetime yet;
   *  '00:00:00' once overdue (the registration has lapsed). */
  nextHeartbeat(e: EpInfo): string {
    if (!e.lifetime || !e.last_seen_unix) return '—';
    return this.hhmmss(e.last_seen_unix + e.lifetime - this.now);
  }

  ngOnInit(): void {
    this.ticker = setInterval(() => { this.now = Math.floor(Date.now() / 1000); }, 1000);
    this.startLongPoll();
    this.http.dbGet(['cloud.dev.mode', 'cloud.endpoint.credentials',
                     'cloud.proxy.device.ui.port', 'cloud.vpn.subnet']).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.devMode = d['cloud.dev.mode'] === true;
          const p = Number(d['cloud.proxy.device.ui.port']);
          if (Number.isFinite(p) && p > 0) this.uiPort = p;
          this.serverTunIp = this.firstHost(String(d['cloud.vpn.subnet'] || ''));
          try {
            const arr = JSON.parse(String(d['cloud.endpoint.credentials'] || '[]'));
            this.creds = Array.isArray(arr) ? arr : [];
          } catch { this.creds = []; }
        }
      },
      error: () => {}
    });
  }

  /** Join an endpoint to its credential record on serial, falling back to the
   *  formatted DM PSK identity. */
  credFor(e: EpInfo): EpCred | undefined {
    return this.creds.find(c => c.serial === e.endpoint)
        || this.creds.find(c => c.identity === e.endpoint);
  }

  /** Render a PSK secret only to admins; mask it otherwise. */
  secret(v?: string): string {
    if (!v) return '—';
    return this.isAdmin ? v : '•••••• (admin only)';
  }

  /**
   * Provision via plain db/set (no bespoke endpoint): write the carrier
   * BS PSK + set cloud.provision.request = serial (the watched trigger).
   * iot-cloudd formats the identity, mints the DM PSK, and stores the
   * per-endpoint credentials.
   */
  provision(): void {
    const serial = this.provSerial.trim();
    const psk = this.provBsPsk.trim().toLowerCase();
    if (!serial) { this.toast.error('Serial number required'); return; }
    if (!/^[0-9a-f]{32}$/.test(psk)) { this.toast.error('BS PSK must be 32 hex chars'); return; }
    this.provisioning = true;
    this.http.dbSet([
      { key: 'cloud.provision.bs.psk', value: psk },
      { key: 'cloud.provision.request', value: serial },
    ]).subscribe({
      next: (r) => {
        this.provisioning = false;
        if (r.ok) {
          this.toast.success((this.editing ? 'Updated PSK for ' : 'Provisioning ') + serial);
          this.provSerial = ''; this.provBsPsk = ''; this.editing = false;
        }
        else this.toast.error(r.err || 'Provision failed');
      },
      error: () => { this.provisioning = false; this.toast.error('Provision failed'); }
    });
  }

  /** Per-row "Edit": pre-fill the provision form with this endpoint's serial so
   *  the operator pastes the device's newly-generated BS PSK and re-provisions.
   *  Re-provisioning UPSERTS (replaces bs.psk.key in place, keeps tun_ip/proxy
   *  port) — the right move after a reflashed SD regenerates its PSK, vs
   *  Remove + re-add which would deprovision the endpoint. */
  editPsk(ep: string): void {
    this.provSerial = ep;
    this.provBsPsk = '';
    this.editing = true;
    try { window.scrollTo({ top: 0, behavior: 'smooth' }); } catch { /* noop */ }
  }

  cancelEdit(): void {
    this.editing = false;
    this.provSerial = '';
    this.provBsPsk = '';
  }

  private startLongPoll(): void {
    const poll = (): void => {
      if (!this.active) return;
      this.http.getCloudEndpoints().subscribe({
        next: (eps) => { this.endpoints = eps; setTimeout(() => poll(), 5000); },
        error: () => { if (this.active) setTimeout(() => poll(), 5000); }
      });
    };
    poll();
  }

  deprovision(ep: string): void {
    this.http.deprovisionEndpoint(ep).subscribe({
      next: (r) => {
        if (r.ok) { this.toast.success('Removed ' + ep); this.endpoints = this.endpoints.filter(e => e.endpoint !== ep); }
        else { this.toast.error(r.err || 'Failed'); }
      },
      error: () => this.toast.error('Deprovision failed')
    });
  }

  ngOnDestroy(): void {
    this.active = false;
    if (this.ticker) clearInterval(this.ticker);
    this.sub.unsubscribe();
  }
}
