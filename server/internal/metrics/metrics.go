package metrics

import (
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"
)

type Registry struct {
	mu       sync.Mutex
	counters map[string]*metric
	gauges   map[string]float64
	started  time.Time
}

type metric struct {
	name   string
	labels map[string]string
	value  float64
}

type Snapshot struct {
	Counters map[string]float64 `json:"counters"`
	Gauges   map[string]float64 `json:"gauges"`
	Uptime   int64              `json:"uptime_seconds"`
}

func New() *Registry {
	return &Registry{
		counters: make(map[string]*metric),
		gauges:   make(map[string]float64),
		started:  time.Now(),
	}
}

func (r *Registry) Inc(name string, labels map[string]string) {
	r.Add(name, labels, 1)
}

func (r *Registry) Add(name string, labels map[string]string, value float64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	key := keyFor(name, labels)
	item, ok := r.counters[key]
	if !ok {
		item = &metric{name: name, labels: copyLabels(labels)}
		r.counters[key] = item
	}
	item.value += value
}

func (r *Registry) Set(name string, value float64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gauges[name] = value
}

func (r *Registry) Snapshot() Snapshot {
	r.mu.Lock()
	defer r.mu.Unlock()
	value := Snapshot{
		Counters: make(map[string]float64, len(r.counters)),
		Gauges:   make(map[string]float64, len(r.gauges)),
		Uptime:   int64(time.Since(r.started).Seconds()),
	}
	for key, item := range r.counters {
		value.Counters[key] = item.value
	}
	for key, item := range r.gauges {
		value.Gauges[key] = item
	}
	return value
}

func (r *Registry) ServeHTTP(writer http.ResponseWriter, _ *http.Request) {
	writer.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
	writer.WriteHeader(http.StatusOK)
	r.WritePrometheus(writer)
}

func (r *Registry) WritePrometheus(writer io.Writer) {
	r.mu.Lock()
	counterKeys := make([]string, 0, len(r.counters))
	for key := range r.counters {
		counterKeys = append(counterKeys, key)
	}
	sort.Strings(counterKeys)
	counters := make([]metric, 0, len(counterKeys))
	for _, key := range counterKeys {
		item := *r.counters[key]
		counters = append(counters, item)
	}
	gaugeKeys := make([]string, 0, len(r.gauges))
	for key := range r.gauges {
		gaugeKeys = append(gaugeKeys, key)
	}
	sort.Strings(gaugeKeys)
	gauges := make(map[string]float64, len(gaugeKeys))
	for _, key := range gaugeKeys {
		gauges[key] = r.gauges[key]
	}
	started := r.started
	r.mu.Unlock()

	for _, item := range counters {
		_, _ = fmt.Fprintf(writer, "%s%s %g\n", item.name, formatLabels(item.labels), item.value)
	}
	for _, name := range gaugeKeys {
		_, _ = fmt.Fprintf(writer, "%s %g\n", name, gauges[name])
	}
	_, _ = fmt.Fprintf(writer, "parcel_process_uptime_seconds %g\n", time.Since(started).Seconds())
}

func keyFor(name string, labels map[string]string) string {
	return name + formatLabels(labels)
}

func formatLabels(labels map[string]string) string {
	if len(labels) == 0 {
		return ""
	}
	keys := make([]string, 0, len(labels))
	for key := range labels {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	parts := make([]string, 0, len(keys))
	for _, key := range keys {
		value := strings.ReplaceAll(labels[key], `\`, `\\`)
		value = strings.ReplaceAll(value, `"`, `\"`)
		value = strings.ReplaceAll(value, "\n", `\n`)
		parts = append(parts, fmt.Sprintf(`%s="%s"`, key, value))
	}
	return "{" + strings.Join(parts, ",") + "}"
}

func copyLabels(labels map[string]string) map[string]string {
	if len(labels) == 0 {
		return nil
	}
	value := make(map[string]string, len(labels))
	for key, item := range labels {
		value[key] = item
	}
	return value
}
