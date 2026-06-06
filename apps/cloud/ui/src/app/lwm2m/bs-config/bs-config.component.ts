import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

@Component({
  selector: 'app-bs-config',
  templateUrl: './bs-config.component.html',
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 8px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 0 0 16px 0; font-size: 14px; font-weight: 600;
         border-bottom: 1px solid #e0e0e0; padding-bottom: 8px; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
    .info-card {
      background: #f0f5ff; border: 1px solid #b3d4ff; border-radius: 4px;
      padding: 12px 16px; font-size: 13px; color: #333;
    }
    .info-card code { background: #d0e4ff; padding: 1px 4px; border-radius: 2px; font-size: 12px; }
  `]
})
export class BsConfigComponent implements OnInit {
  bsForm: FormGroup;
  provForm: FormGroup;
  loading = true;
  savingBs = false;
  provisioning = false;
  provMsg = '';
  provOk = false;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(
    private http: HttpsvcService,
    fb: FormBuilder,
    private session: SessionService,
    private toast: ToastService
  ) {
    this.bsForm = fb.group({
      endpoint:      ['urn:dev:gateway-'],
      bs_uri:        ['coaps://0.0.0.0:5684'],
      security_mode: ['PSK'],
      psk_id:        ['iot-client'],
      psk_key:       [''],
      dm_uri:        ['coaps://0.0.0.0:5683'],
    });
    this.provForm = fb.group({
      endpoint:     ['urn:dev:'],
      sec_uri:      ['coaps://0.0.0.0:5683'],
      sec_bs:       [1],
      sec_mode:     [0],
      sec_identity: ['iot-client'],
      sec_key:      [''],
      sec_ssid:     [0],
      srv_lifetime:  [86400],
      srv_binding:   ['U'],
      lwm2m_version: ['1.1'],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'cloud.bs.endpoint', 'cloud.bs.uri', 'cloud.bs.security.mode',
      'cloud.bs.psk.id', 'cloud.bs.psk.key', 'cloud.dm.uri'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.bsForm.patchValue({
            endpoint:      d['cloud.bs.endpoint']      || 'urn:dev:gateway-',
            bs_uri:        d['cloud.bs.uri']           || 'coaps://0.0.0.0:5684',
            security_mode: d['cloud.bs.security.mode'] || 'PSK',
            psk_id:        d['cloud.bs.psk.id']        || 'iot-client',
            psk_key:       d['cloud.bs.psk.key']       || '',
            dm_uri:        d['cloud.dm.uri']           || 'coaps://0.0.0.0:5683',
          });
          this.fillFromServer();
        }
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
  }

  saveBs(): void {
    this.savingBs = true;
    const v = this.bsForm.value;
    this.http.dbSet([
      { key: 'cloud.bs.endpoint',      value: v.endpoint },
      { key: 'cloud.bs.uri',           value: v.bs_uri },
      { key: 'cloud.bs.security.mode', value: v.security_mode },
      { key: 'cloud.bs.psk.id',        value: v.psk_id },
      { key: 'cloud.bs.psk.key',       value: v.psk_key },
      { key: 'cloud.dm.uri',           value: v.dm_uri },
    ]).subscribe({
      next: (r) => {
        this.savingBs = false;
        if (r.ok) this.toast.success('BS config saved');
        else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.savingBs = false; this.toast.error('Save failed'); }
    });
  }

  /// Copy server-config into the provision form.
  fillFromServer(): void {
    const v = this.bsForm.value;
    this.provForm.patchValue({
      sec_uri:      v.dm_uri,
      sec_identity: v.psk_id,
      sec_key:      v.psk_key,
      sec_mode:     v.security_mode === 'None' ? 3 : 0,
    });
  }

  /// Write cloud.provision.request = endpoint name AND update
  /// cloud.provision.configs with the per-endpoint Security Object
  /// config.  iot-cloudd watches cloud.provision.request and reads
  /// cloud.provision.configs for the TLV payload.
  provision(): void {
    this.provisioning = true;
    this.provMsg = '';
    const v = this.provForm.value;
    const ep = (v.endpoint || '').trim();
    if (!ep) {
      this.provisioning = false;
      this.provMsg = 'Endpoint name is required';
      this.provOk = false;
      return;
    }

    // Build the per-endpoint config entry (dots in keys).
    // Security Object (OID 0) + Server Object (OID 1).
    const entry: Record<string, unknown> = {
      'sec.uri':      v.sec_uri,
      'sec.bs':       v.sec_bs,
      'sec.mode':     v.sec_mode,
      'sec.identity': v.sec_identity,
      'sec.key':      v.sec_key,
      'sec.ssid':     v.sec_ssid,
      'srv.lifetime':   v.srv_lifetime,
      'srv.binding':    v.srv_binding,
      'lwm2m.version':  v.lwm2m_version,
    };

    // Read existing configs, add/update this endpoint, write back
    this.http.dbGet(['cloud.provision.configs']).subscribe({
      next: (r) => {
        let configs: Record<string, unknown> = {};
        if (r.ok && r.data) {
          const raw = (r.data as Record<string, unknown>)['cloud.provision.configs'];
          if (typeof raw === 'string' && raw) {
            try { configs = JSON.parse(raw); } catch (_) {}
          }
        }
        configs[ep] = entry;

        this.http.dbSet([
          { key: 'cloud.provision.request', value: ep },
          { key: 'cloud.provision.configs', value: JSON.stringify(configs) },
        ]).subscribe({
          next: (s) => {
            this.provisioning = false;
            if (s.ok) {
              this.provMsg = `Provisioning ${ep}… tunnel IP + proxy port assigned.`;
              this.provOk = true;
              this.toast.success('Provision request sent for ' + ep);
            } else {
              this.provMsg = s.err || 'Provision request failed';
              this.provOk = false;
              this.toast.error(this.provMsg);
            }
          },
          error: (err) => {
            this.provisioning = false;
            this.provMsg = 'Network error: ' + (err?.message || 'unknown');
            this.provOk = false;
            this.toast.error(this.provMsg);
          }
        });
      },
      error: (err) => {
        this.provisioning = false;
        this.provMsg = 'Failed to read configs: ' + (err?.message || 'unknown');
        this.provOk = false;
        this.toast.error(this.provMsg);
      }
    });
  }
}
