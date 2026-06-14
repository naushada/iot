import { Component, OnInit } from '@angular/core';
import { HttpsvcService } from '../../../common/httpsvc.service';

/** One verbose forwarding (DNAT) rule row, one per provisioned endpoint. */
interface FwdRule {
  endpoint:   string;
  srcIp:      string;   // where the operator connects (the cloud host)
  srcPort:    number;   // published proxy_port on the cloud
  dstIp:      string;   // device tunnel IP (openvpn-assigned, else allocated)
  dstPort:    number;   // device UI port reached over the tunnel
  proto:      string;   // transport (tcp for the UI DNAT)
  registered: boolean;
}

// getCloudEndpoints() passes the cloud.endpoints JSON through verbatim, so
// dev_tun_ip rides along even though the service's declared type omits it.
interface RawEp { endpoint:string; tun_ip:string; dev_tun_ip?:string; proxy_port:number; registered:boolean; }

@Component({
  selector: 'app-device-forwarding',
  template: `
    <div class="page">
      <h3>Device Forwarding Rules</h3>
      <p class="sub">
        Per-device DNAT installed by iot-cloudd: each rule forwards the cloud's
        published proxy port to the device's web UI over the VPN tunnel
        (<code>cloud:proxy_port → dev_tun_ip:{{ uiPort }}</code>).
      </p>

      <clr-datagrid *ngIf="rules.length">
        <clr-dg-column>Endpoint</clr-dg-column>
        <clr-dg-column>Source IP</clr-dg-column>
        <clr-dg-column>Source Port</clr-dg-column>
        <clr-dg-column>Destination IP</clr-dg-column>
        <clr-dg-column>Destination Port</clr-dg-column>
        <clr-dg-column>Protocol</clr-dg-column>
        <clr-dg-column>State</clr-dg-column>
        <clr-dg-row *clrDgItems="let r of rules">
          <clr-dg-cell><code>{{ r.endpoint }}</code></clr-dg-cell>
          <clr-dg-cell>{{ r.srcIp }}</clr-dg-cell>
          <clr-dg-cell>{{ r.srcPort }}</clr-dg-cell>
          <clr-dg-cell>{{ r.dstIp || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ r.dstPort }}</clr-dg-cell>
          <clr-dg-cell><span class="label">{{ r.proto }}</span></clr-dg-cell>
          <clr-dg-cell>
            <app-status-badge [label]="r.registered?'online':'offline'"
                              [state]="r.registered?'connected':'exited'"></app-status-badge>
          </clr-dg-cell>
        </clr-dg-row>
        <clr-dg-footer>{{ rules.length }} forwarding rule(s)</clr-dg-footer>
      </clr-datagrid>
      <p *ngIf="!rules.length" class="empty">No devices provisioned — no forwarding rules yet.</p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; } h3 { color: #333; margin: 0 0 8px 0; font-size: 16px; font-weight: 600; }
    .sub { color: #666; font-size: 13px; margin: 0 0 20px 0; }
    .empty { color: #888; }
  `]
})
export class DeviceForwardingComponent implements OnInit {
  rules: FwdRule[] = [];
  uiPort = 80;
  // The cloud host the operator reaches the proxy on — the same origin this
  // UI is served from. Shown as the rule's source IP.
  private cloudHost = window.location.hostname;

  constructor(private http: HttpsvcService) {}

  ngOnInit(): void {
    this.http.dbGet(['cloud.proxy.device.ui.port']).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const p = Number((r.data as Record<string, unknown>)['cloud.proxy.device.ui.port']);
          if (Number.isFinite(p) && p > 0) this.uiPort = p;
        }
        this.loadRules();
      },
      error: () => this.loadRules()
    });
  }

  private loadRules(): void {
    this.http.getCloudEndpoints().subscribe({
      next: (eps) => {
        this.rules = (eps as RawEp[]).map(e => ({
          endpoint:   e.endpoint,
          srcIp:      this.cloudHost,
          srcPort:    e.proxy_port,
          dstIp:      e.dev_tun_ip || e.tun_ip,
          dstPort:    this.uiPort,
          proto:      'tcp',
          registered: e.registered,
        }));
      }
    });
  }
}
