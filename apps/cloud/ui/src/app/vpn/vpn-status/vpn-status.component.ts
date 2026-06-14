import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../../common/httpsvc.service';

/// Cloud VPN status = the OpenVPN **server** view (the cloud runs the server, not
/// a client). The device's vpn.* client keys (assigned ip/gateway/dns/pid) never
/// populate here, so this shows server state, listen config, and the connected
/// clients instead — sourced from services.cloud.openvpn.server.state +
/// cloud.vpn.* + cloud.vpn.connected (the live client list from the openvpn mgmt
/// ROUTING TABLE).
@Component({
  selector: 'app-vpn-status',
  template: `
    <div class="page">
      <h3>VPN Server Status</h3>
      <clr-datagrid *ngIf="!loading">
        <clr-dg-column>Property</clr-dg-column>
        <clr-dg-column>Value</clr-dg-column>

        <clr-dg-row>
          <clr-dg-cell>Server State
            <app-ds-hint *dsDebug key="services.cloud.openvpn.server.state"></app-ds-hint>
          </clr-dg-cell>
          <clr-dg-cell><app-status-badge [label]="serverState" [state]="serverState"></app-status-badge></clr-dg-cell>
        </clr-dg-row>
        <clr-dg-row>
          <clr-dg-cell>Listen
            <app-ds-hint *dsDebug key="cloud.vpn.listen.port"></app-ds-hint>
          </clr-dg-cell>
          <clr-dg-cell>{{ listen }}</clr-dg-cell>
        </clr-dg-row>
        <clr-dg-row>
          <clr-dg-cell>Tunnel Subnet
            <app-ds-hint *dsDebug key="cloud.vpn.subnet"></app-ds-hint>
          </clr-dg-cell>
          <clr-dg-cell>{{ subnet || '—' }}</clr-dg-cell>
        </clr-dg-row>
        <clr-dg-row>
          <clr-dg-cell>Cipher
            <app-ds-hint *dsDebug key="cloud.vpn.cipher"></app-ds-hint>
          </clr-dg-cell>
          <clr-dg-cell>{{ cipher || '—' }}</clr-dg-cell>
        </clr-dg-row>
        <clr-dg-row>
          <clr-dg-cell>TUN Device
            <app-ds-hint *dsDebug key="cloud.vpn.dev"></app-ds-hint>
          </clr-dg-cell>
          <clr-dg-cell>{{ dev || '—' }}</clr-dg-cell>
        </clr-dg-row>
        <clr-dg-row>
          <clr-dg-cell>Connected Clients
            <app-ds-hint *dsDebug key="cloud.vpn.connected"></app-ds-hint>
          </clr-dg-cell>
          <clr-dg-cell>
            <span *ngFor="let c of clients" class="chip"><code>{{ c }}</code></span>
            <span *ngIf="!clients.length" style="color:#888;">none</span>
          </clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ clients.length }} client(s) connected</clr-dg-footer>
      </clr-datagrid>
      <p *ngIf="loading">Loading…</p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    .chip { display: inline-block; margin: 2px 6px 2px 0; padding: 1px 8px;
            background: #eef2f7; border-radius: 10px; font-size: 12px; }
  `]
})
export class VpnStatusComponent implements OnInit, OnDestroy {
  serverState = 'unknown';
  proto = ''; port = 0; subnet = ''; cipher = ''; dev = '';
  clients: string[] = [];
  loading = true;
  private sub = new Subscription();
  private active = true;

  /// "TCP :1194" — proto with the openvpn -server/-client suffix dropped.
  get listen(): string {
    if (!this.port) return '—';
    const p = this.proto.replace(/-server$|-client$/, '').toUpperCase();
    return (p ? p + ' ' : '') + ':' + this.port;
  }

  constructor(private http: HttpsvcService) {}

  ngOnInit(): void { this.refresh(); this.poll(); }
  ngOnDestroy(): void { this.active = false; this.sub.unsubscribe(); }

  private refresh(): void {
    this.sub.add(this.http.dbGet([
      'services.cloud.openvpn.server.state', 'cloud.vpn.proto',
      'cloud.vpn.listen.port', 'cloud.vpn.subnet', 'cloud.vpn.cipher',
      'cloud.vpn.dev', 'cloud.vpn.connected',
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.serverState = String(d['services.cloud.openvpn.server.state'] || 'unknown');
          this.proto  = String(d['cloud.vpn.proto'] || '');
          this.port   = Number(d['cloud.vpn.listen.port']) || 0;
          this.subnet = String(d['cloud.vpn.subnet'] || '');
          this.cipher = String(d['cloud.vpn.cipher'] || '');
          this.dev    = String(d['cloud.vpn.dev'] || '');
          try {
            const a = JSON.parse(String(d['cloud.vpn.connected'] || '[]'));
            this.clients = Array.isArray(a) ? a : [];
          } catch { this.clients = []; }
        }
        this.loading = false;
      },
      error: () => { this.loading = false; }
    }));
  }

  /// Live updates: long-poll the connected-client list; on each change/timeout
  /// re-read the row data, then poll again.
  private poll(): void {
    if (!this.active) return;
    this.sub.add(this.http.dbGetLongPoll('cloud.vpn.connected', 30).subscribe({
      next: () => { this.refresh(); if (this.active) this.poll(); },
      error: () => { if (this.active) setTimeout(() => this.poll(), 5000); }
    }));
  }
}
