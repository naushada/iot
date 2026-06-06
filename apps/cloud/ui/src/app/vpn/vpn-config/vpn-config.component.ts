import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

@Component({
  selector: 'app-vpn-config',
  templateUrl: './vpn-config.component.html',
  styleUrls: ['./vpn-config.component.scss']
})
export class VpnConfigComponent implements OnInit {
  form: FormGroup;
  loading = true;
  get isAdmin(): boolean { return this.session.isAdmin; }
  saving = false;

  constructor(
    private http: HttpsvcService,
    fb: FormBuilder,
    private session: SessionService,
    private toast: ToastService
  ) {
    this.form = fb.group({
      subnet:     ['10.9.0.0/24'],
      port_next:  [5001],
      ca_crt:     ['/etc/iot/vpn/ca/ca.crt'],
      ca_key:     ['/run/secrets/iot-ca-key/ca.key'],
      server_crt: ['/etc/iot/vpn/server.crt'],
      server_key: ['/etc/iot/vpn/server.key'],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'cloud.vpn.subnet', 'cloud.vpn.port.next',
      'cloud.vpn.ca.crt', 'cloud.vpn.ca.key',
      'cloud.vpn.server.crt', 'cloud.vpn.server.key'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.form.patchValue({
            subnet:     d['cloud.vpn.subnet']      || '10.9.0.0/24',
            port_next:  d['cloud.vpn.port.next']   || 5001,
            ca_crt:     d['cloud.vpn.ca.crt']      || '/etc/iot/vpn/ca/ca.crt',
            ca_key:     d['cloud.vpn.ca.key']      || '/run/secrets/iot-ca-key/ca.key',
            server_crt: d['cloud.vpn.server.crt']  || '/etc/iot/vpn/server.crt',
            server_key: d['cloud.vpn.server.key']  || '/etc/iot/vpn/server.key',
          });
        }
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
  }

  save(): void {
    this.saving = true;
    const v = this.form.value;
    this.http.dbSet([
      { key: 'cloud.vpn.subnet',      value: v.subnet },
      { key: 'cloud.vpn.port.next',   value: v.port_next },
      { key: 'cloud.vpn.ca.crt',      value: v.ca_crt },
      { key: 'cloud.vpn.ca.key',      value: v.ca_key },
      { key: 'cloud.vpn.server.crt',  value: v.server_crt },
      { key: 'cloud.vpn.server.key',  value: v.server_key },
    ]).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) this.toast.success('VPN config saved');
        else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
