import { Component } from '@angular/core';
import { HttpsvcService } from '../../common/httpsvc.service';
import { ToastService } from '../../common/toast.service';

@Component({
  selector: 'app-provision',
  template: `
    <div class="page">
      <h3>Provision Device</h3>
      <div class="form-grid" style="align-items:end;">
        <clr-input-container>
          <label>Endpoint Name</label>
          <input clrInput [(ngModel)]="endpoint" placeholder="urn:dev:gateway-42" />
        </clr-input-container>
        <div class="btn-cell"><button class="btn btn-primary" (click)="provision()" [disabled]="saving || !endpoint">{{saving?'Provisioning…':'Provision'}}</button></div>
      </div>
      <div *ngIf="result" style="margin-top:24px;padding:16px;background:#e8f5e9;border-radius:6px;">
        <h4 style="margin:0 0 12px 0;color:#2e7d32;">Provisioned Successfully</h4>
        <table class="table table-compact table-borderless">
          <tr><td class="label-col">Endpoint</td><td><code>{{result.endpoint}}</code></td></tr>
          <tr><td class="label-col">Tunnel IP</td><td>{{result.tun_ip}}</td></tr>
          <tr><td class="label-col">Proxy Port</td><td>{{result.proxy_port}}</td></tr>
        </table>
      </div>
    </div>
  `,
  styles: [`.page{padding:24px;} h3{color:#333;margin:0 0 20px 0;font-size:16px;font-weight:600;} h4{font-size:14px;} .btn-cell{display:flex;align-items:flex-end;}`]
})
export class ProvisionComponent {
  endpoint = ''; saving = false;
  result: any = null;

  constructor(private http: HttpsvcService, private toast: ToastService) {}

  provision(): void {
    this.saving = true; this.result = null;
    this.http.provisionEndpoint(this.endpoint).subscribe({
      next: (r) => { this.saving = false; this.result = r; this.toast.success('Provisioned ' + this.endpoint); },
      error: (e) => { this.saving = false; this.toast.error(e?.error?.err || 'Provision failed'); }
    });
  }
}
