import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { ToastService } from '../../common/toast.service';

interface EpInfo { endpoint:string; tun_ip:string; proxy_port:number; registered:boolean; }

@Component({
  selector: 'app-endpoint-list',
  template: `
    <div class="page">
      <div class="header-row">
        <h3>Endpoints</h3>
        <a class="btn btn-primary btn-sm" routerLink="/main" (click)="$event.preventDefault(); menu('provision')">+ Provision</a>
      </div>

      <clr-datagrid>
        <clr-dg-column>Endpoint</clr-dg-column>
        <clr-dg-column>State</clr-dg-column>
        <clr-dg-column>Tunnel IP</clr-dg-column>
        <clr-dg-column>Proxy Port</clr-dg-column>
        <clr-dg-column>Device UI</clr-dg-column>
        <clr-dg-column *ngIf="isAdmin">Actions</clr-dg-column>

        <clr-dg-row *clrDgItems="let e of endpoints">
          <clr-dg-cell><code>{{e.endpoint}}</code></clr-dg-cell>
          <clr-dg-cell>
            <app-status-badge [label]="e.registered?'online':'offline'"
              [state]="e.registered?'connected':'exited'"></app-status-badge>
          </clr-dg-cell>
          <clr-dg-cell><code>{{e.tun_ip}}</code></clr-dg-cell>
          <clr-dg-cell>{{e.proxy_port}}</clr-dg-cell>
          <clr-dg-cell>
            <a class="btn btn-sm" target="_blank" rel="noopener"
               [href]="'https://' + windowHost + ':' + e.proxy_port + '/'"
               *ngIf="e.registered && e.proxy_port">
              Launch UI <clr-icon shape="pop-out" size="12"></clr-icon>
            </a>
            <span *ngIf="!e.registered" class="hint">offline</span>
          </clr-dg-cell>
          <clr-dg-cell *ngIf="isAdmin">
            <button class="btn btn-sm btn-danger" (click)="deprovision(e.endpoint)">Remove</button>
          </clr-dg-cell>
        </clr-dg-row>

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
  `]
})
export class EndpointListComponent implements OnInit, OnDestroy {
  endpoints: EpInfo[] = [];
  windowHost = window.location.hostname;
  isAdmin = true; // TODO: from SessionService
  private sub = new Subscription(); private active = true;

  constructor(private http: HttpsvcService, private toast: ToastService) {}

  ngOnInit(): void { this.startLongPoll(); }

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
