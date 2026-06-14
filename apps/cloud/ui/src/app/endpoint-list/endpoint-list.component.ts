import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';

interface EpInfo { endpoint:string; tun_ip:string; dev_tun_ip?:string; proxy_port:number; registered:boolean; }

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
        <div class="card-header">Provision a device</div>
        <div class="card-block">
          <div class="form-grid">
            <clr-input-container>
              <label>Serial Number</label>
              <input clrInput [(ngModel)]="provSerial" style="width:100%;"
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
            {{ provisioning ? 'Provisioning…' : 'Provision' }}
          </button>
          <span *ngIf="!devMode" class="hint" style="margin-left:12px;">
            Note: enable <code>cloud.dev.mode</code> to store credentials during commissioning.
          </span>
        </div>
      </div>

      <clr-datagrid>
        <clr-dg-column>Endpoint</clr-dg-column>
        <clr-dg-column>State</clr-dg-column>
        <clr-dg-column>Tunnel IP</clr-dg-column>
        <clr-dg-column>Device Tunnel IP</clr-dg-column>
        <clr-dg-column>Proxy Port</clr-dg-column>
        <clr-dg-column>Device UI</clr-dg-column>
        <clr-dg-column *ngIf="isAdmin">Actions</clr-dg-column>

        <clr-dg-row *clrDgItems="let e of endpoints">
          <clr-dg-cell><code>{{e.endpoint}}</code></clr-dg-cell>
          <clr-dg-cell>
            <app-status-badge [label]="e.registered?'online':'offline'"
              [state]="e.registered?'connected':'exited'"></app-status-badge>
          </clr-dg-cell>
          <clr-dg-cell><code>{{ serverTunIp || '—' }}</code></clr-dg-cell>
          <clr-dg-cell><code>{{ e.dev_tun_ip || '—' }}</code></clr-dg-cell>
          <clr-dg-cell>{{e.proxy_port}}</clr-dg-cell>
          <clr-dg-cell>
            <!-- Launch UI only when the device's VPN tunnel is up (dev_tun_ip).
                 Same-origin path-scoped reverse proxy: iot-httpd proxies
                 /dev/<ep>/ over the tun to the device UI (one HTTPS origin,
                 per-device cookie isolation). Requires iot-httpd to share
                 iot-cloudd's netns — see apps/docs/tdd-device-ui-path-proxy.md. -->
            <a class="btn btn-sm" target="_blank" rel="noopener"
               [href]="launchUrl(e.endpoint)"
               *ngIf="e.registered && e.dev_tun_ip">
              Launch UI <clr-icon shape="pop-out" size="12"></clr-icon>
            </a>
            <span *ngIf="!e.registered" class="hint">offline</span>
            <span *ngIf="e.registered && !e.dev_tun_ip" class="hint">VPN down</span>
          </clr-dg-cell>
          <clr-dg-cell *ngIf="isAdmin">
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
  endpoints: EpInfo[] = [];
  creds: EpCred[] = [];
  windowHost = window.location.hostname;
  // Device UI port reached over the VPN (cloud.proxy.device.ui.port,
  // default 80). Shown in the per-endpoint "VPN forwarding rule" line.
  uiPort = 80;
  // VPN server's tunnel IP (first host of cloud.vpn.subnet, e.g. 10.9.0.1) —
  // the server end of every tunnel. Shown in the "Tunnel IP" column.
  serverTunIp = '';
  // PSK provisioning (task O).
  provSerial = ''; provBsPsk = ''; provisioning = false; devMode = false;
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

  constructor(private http: HttpsvcService, private session: SessionService, private toast: ToastService) {}

  ngOnInit(): void {
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
        if (r.ok) { this.toast.success('Provisioning ' + serial); this.provSerial = ''; this.provBsPsk = ''; }
        else this.toast.error(r.err || 'Provision failed');
      },
      error: () => { this.provisioning = false; this.toast.error('Provision failed'); }
    });
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

  ngOnDestroy(): void { this.active = false; this.sub.unsubscribe(); }
}
