import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { ToastService } from '../../common/toast.service';

interface EpInfo { endpoint:string; tun_ip:string; proxy_port:number; registered:boolean; }

@Component({
  selector: 'app-endpoint-list',
  template: `
    <div class="page">
      <h3>Endpoints</h3>
      <clr-datagrid>
        <clr-dg-column>Endpoint</clr-dg-column>
        <clr-dg-column>State</clr-dg-column>
        <clr-dg-column>Tunnel IP</clr-dg-column>
        <clr-dg-column>Proxy Port</clr-dg-column>
        <clr-dg-column>Actions</clr-dg-column>
        <clr-dg-row *clrDgItems="let e of endpoints">
          <clr-dg-cell><code>{{e.endpoint}}</code></clr-dg-cell>
          <clr-dg-cell><app-status-badge [label]="e.registered?'online':'offline'" [state]="e.registered?'connected':'exited'"></app-status-badge></clr-dg-cell>
          <clr-dg-cell>{{e.tun_ip}}</clr-dg-cell>
          <clr-dg-cell>{{e.proxy_port}}</clr-dg-cell>
          <clr-dg-cell>
            <a class="btn btn-sm" [href]="'https://localhost:'+e.proxy_port+'/'" target="_blank" *ngIf="e.registered">Launch UI</a>
            <button class="btn btn-sm btn-danger" (click)="deprovision(e.endpoint)">Remove</button>
          </clr-dg-cell>
        </clr-dg-row>
        <clr-dg-footer>{{endpoints.length}} endpoints</clr-dg-footer>
      </clr-datagrid>
    </div>
  `,
  styles: [`.page{padding:24px;} h3{color:#333;margin:0 0 16px 0;font-size:16px;font-weight:600;}`]
})
export class EndpointListComponent implements OnInit, OnDestroy {
  endpoints: EpInfo[] = [];
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
