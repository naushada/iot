import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../../common/httpsvc.service';

@Component({
  selector: 'app-vpn-config',
  templateUrl: './vpn-config.component.html',
  styleUrls: ['./vpn-config.component.scss']
})
export class VpnConfigComponent implements OnInit {
  form: FormGroup;
  loading = true;
  saving = false;
  msg = '';

  constructor(private http: HttpsvcService, fb: FormBuilder) {
    this.form = fb.group({
      remote_host:  [''],
      remote_port:  [1194],
      remote_proto: ['udp'],
      cert_path:    [''],
      key_path:     [''],
      ca_path:      [''],
      cipher:       ['AES-256-GCM'],
      dev:          ['tun'],
      mgmt_port:    [7505],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'vpn.remote.host', 'vpn.remote.port', 'vpn.remote.proto',
      'vpn.cert.path', 'vpn.key.path', 'vpn.ca.path',
      'vpn.cipher', 'vpn.dev', 'vpn.mgmt.port'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.form.patchValue({
            remote_host:  d['vpn.remote.host']  || '',
            remote_port:  d['vpn.remote.port']  || 1194,
            remote_proto: d['vpn.remote.proto'] || 'udp',
            cert_path:    d['vpn.cert.path']    || '',
            key_path:     d['vpn.key.path']     || '',
            ca_path:      d['vpn.ca.path']      || '',
            cipher:       d['vpn.cipher']       || 'AES-256-GCM',
            dev:          d['vpn.dev']          || 'tun',
            mgmt_port:    d['vpn.mgmt.port']    || 7505,
          });
        }
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
  }

  save(): void {
    this.saving = true; this.msg = '';
    const v = this.form.value;
    this.http.dbSet([
      { key: 'vpn.remote.host',  value: v.remote_host },
      { key: 'vpn.remote.port',  value: v.remote_port },
      { key: 'vpn.remote.proto', value: v.remote_proto },
      { key: 'vpn.cert.path',    value: v.cert_path },
      { key: 'vpn.key.path',     value: v.key_path },
      { key: 'vpn.ca.path',      value: v.ca_path },
      { key: 'vpn.cipher',       value: v.cipher },
      { key: 'vpn.dev',          value: v.dev },
      { key: 'vpn.mgmt.port',    value: v.mgmt_port },
    ]).subscribe({
      next: (r) => {
        this.saving = false;
        this.msg = r.ok ? 'Saved.' : ('Error: ' + (r.err || 'unknown'));
      },
      error: (e) => { this.saving = false; this.msg = 'Save failed.'; }
    });
  }
}
