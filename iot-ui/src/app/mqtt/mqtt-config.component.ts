import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';

/// MQTT mirror config — broker + topic + "mirror telemetry" toggle, written to
/// mqtt.* (read by the iot-mqttd daemon). A SEPARATE, VPN-independent plane
/// (device → broker over the WAN). The password is write-only: blank on save
/// keeps the stored value.
@Component({
  selector: 'app-mqtt-config',
  template: `
    <div class="page">
      <h3>MQTT Mirror</h3>
      <p class="hint">
        Publish vehicle telemetry to your own MQTT broker. Topic:
        <code>&lt;serial&gt;/{{ form.value.suffix || 'telemetry' }}</code>. The
        client stays parked until a broker host is set.
      </p>
      <form [formGroup]="form" (ngSubmit)="save()">
        <div class="form-grid">
          <clr-input-container>
            <label>Broker Host</label>
            <input clrInput formControlName="host" [disabled]="!isAdmin" placeholder="mqtt.example.com" />
            <clr-control-helper *dsDebug><app-ds-hint key="mqtt.broker.host"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Port</label>
            <input clrInput type="number" formControlName="port" [disabled]="!isAdmin" />
            <clr-control-helper *dsDebug><app-ds-hint key="mqtt.broker.port"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Username</label>
            <input clrInput formControlName="user" [disabled]="!isAdmin" placeholder="(optional)" />
            <clr-control-helper *dsDebug><app-ds-hint key="mqtt.broker.user"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Password</label>
            <input clrInput type="password" formControlName="pass" [disabled]="!isAdmin"
                   autocomplete="new-password" placeholder="(unchanged)" />
            <clr-control-helper *dsDebug><app-ds-hint key="mqtt.broker.pass"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Topic Suffix</label>
            <input clrInput formControlName="suffix" [disabled]="!isAdmin" placeholder="telemetry" />
            <clr-control-helper *dsDebug><app-ds-hint key="mqtt.topic.suffix"></app-ds-hint></clr-control-helper>
          </clr-input-container>
        </div>
        <clr-checkbox-container>
          <clr-checkbox-wrapper>
            <input type="checkbox" clrCheckbox formControlName="mirror" [disabled]="!isAdmin" />
            <label>Mirror vehicle telemetry to the broker</label>
          </clr-checkbox-wrapper>
          <clr-control-helper *dsDebug><app-ds-hint key="mqtt.mirror.enable"></app-ds-hint></clr-control-helper>
        </clr-checkbox-container>
        <div style="margin-top:16px;" *ngIf="isAdmin">
          <button type="submit" class="btn btn-primary" [disabled]="saving">
            {{ saving ? 'Saving…' : 'Save' }}
          </button>
        </div>
      </form>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 12px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 16px 0; }
  `]
})
export class MqttConfigComponent implements OnInit {
  form: FormGroup;
  saving = false;
  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, fb: FormBuilder,
              private session: SessionService, private toast: ToastService) {
    this.form = fb.group({
      host: [''], port: [1883], user: [''], pass: [''], suffix: ['telemetry'], mirror: [false],
    });
  }

  ngOnInit(): void {
    this.http.dbGet(['mqtt.broker.host', 'mqtt.broker.port', 'mqtt.broker.user',
                     'mqtt.topic.suffix', 'mqtt.mirror.enable'])
      .subscribe(r => {
        if (!r.ok || !r.data) return;
        const d = r.data;
        this.form.patchValue({
          host:   (d['mqtt.broker.host'] as string) || '',
          port:   (d['mqtt.broker.port'] as number) || 1883,
          user:   (d['mqtt.broker.user'] as string) || '',
          suffix: (d['mqtt.topic.suffix'] as string) || 'telemetry',
          mirror: d['mqtt.mirror.enable'] === true,
        });
      });
  }

  save(): void {
    if (!this.isAdmin) return;
    const v = this.form.value;
    const pairs: { key: string; value: unknown }[] = [
      { key: 'mqtt.broker.host',   value: v.host || '' },
      { key: 'mqtt.broker.port',   value: Number(v.port) || 1883 },
      { key: 'mqtt.broker.user',   value: v.user || '' },
      { key: 'mqtt.topic.suffix',  value: v.suffix || 'telemetry' },
      { key: 'mqtt.mirror.enable', value: !!v.mirror },
    ];
    if (v.pass) pairs.push({ key: 'mqtt.broker.pass', value: v.pass });  // only when entered
    this.saving = true;
    this.http.dbSet(pairs).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) this.toast.success('MQTT settings saved'); else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
