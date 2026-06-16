import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { DataStoreService } from '../../common/datastore.service';
import { HttpClient } from '@angular/common/http';
import { environment } from '../../environments/environment';

interface EpInfo { endpoint:string; tun_ip:string; dev_tun_ip?:string; proxy_port:number; registered:boolean; }

@Component({
  selector: 'app-dashboard',
  template: `
    <div class="page">
      <h3>Cloud Dashboard</h3>
      <div class="clr-row">
        <div class="clr-col-lg-3 clr-col-md-6 clr-col-12" *ngFor="let c of cards">
          <div class="card status-card" [ngClass]="c.cls">
            <clr-icon [attr.shape]="c.icon" size="36"></clr-icon>
            <div class="card-value">{{ c.value }}</div>
            <div class="card-label">{{ c.label }}</div>
          </div>
        </div>
      </div>
      <h4 style="margin-top:24px;">Endpoints</h4>
      <clr-datagrid *ngIf="endpoints.length">
        <clr-dg-column>Endpoint</clr-dg-column>
        <clr-dg-column>State</clr-dg-column>
        <clr-dg-column>Tunnel IP</clr-dg-column>
        <clr-dg-column>Device Tunnel IP</clr-dg-column>
        <clr-dg-column>Port</clr-dg-column>
        <clr-dg-row *clrDgItems="let e of endpoints">
          <clr-dg-cell><code>{{e.endpoint}}</code></clr-dg-cell>
          <clr-dg-cell><app-status-badge [label]="e.registered?'online':'offline'" [state]="e.registered?'connected':'exited'"></app-status-badge></clr-dg-cell>
          <clr-dg-cell>{{ serverTunIp || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ e.dev_tun_ip || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{e.proxy_port}}</clr-dg-cell>
        </clr-dg-row>
        <clr-dg-footer>{{endpoints.length}} endpoints</clr-dg-footer>
      </clr-datagrid>
      <p *ngIf="!endpoints.length" style="color:#888;">No devices provisioned yet.</p>
    </div>
  `,
  styles: [`.page{padding:24px;} h3,h4{color:#333;margin:0 0 16px 0;} h4{font-size:14px;margin-top:20px;}
    .clr-row > [class*="clr-col"]{margin-bottom:1rem;}`]
})
export class DashboardComponent implements OnInit, OnDestroy {
  endpoints: EpInfo[] = [];
  private sub = new Subscription();
  // VPN server's tunnel IP (first host of cloud.vpn.subnet) — the server end of
  // every tunnel; shown in the "Tunnel IP" column.
  serverTunIp = '';
  cards: {label:string;value:number;icon:string;cls:string}[] = [
    {label:'Online',value:0,icon:'check-circle',cls:'connected'},
    {label:'Offline',value:0,icon:'times-circle',cls:'disconnected'},
    {label:'Total',value:0,icon:'devices',cls:'starting'},
  ];

  constructor(private http: HttpsvcService, private ds: DataStoreService) {}

  ngOnInit(): void {
    // Server tunnel IP is derived from cloud.vpn.subnet — read it from the
    // shared prefetched cache and stay live off the appglobal store.
    this.applySubnet(this.ds.getString('cloud.vpn.subnet'));
    this.sub.add(this.ds.observe('cloud.vpn.subnet')
      .subscribe(v => this.applySubnet(typeof v === 'string' ? v : '')));
    this.http.getCloudEndpoints().subscribe({
      next: (eps) => {
        this.endpoints = eps;
        this.cards[0].value = eps.filter(e=>e.registered).length;
        this.cards[1].value = eps.filter(e=>!e.registered).length;
        this.cards[2].value = eps.length;
      }
    });
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  private applySubnet(subnet: string): void {
    const base = String(subnet || '').split('/')[0];
    const o = base.split('.');
    if (o.length === 4) { o[3] = String((Number(o[3]) || 0) + 1); this.serverTunIp = o.join('.'); }
  }
}
