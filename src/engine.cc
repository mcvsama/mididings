/*
 * mididings
 *
 * Copyright (C) 2008-2014  Dominic Sacré  <dominic.sacre@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "config.hh"
#include "engine.hh"

#include "util/python.hh"
#include "units/base.hh"
#include "units/engine.hh"

#include <boost/thread/thread.hpp>
#include <boost/bind.hpp>

#include <iostream>
#include <algorithm>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include "util/debug.hh"


namespace mididings {


#ifdef ENABLE_BENCHMARK
Engine::hrclock::duration Engine::cycles_duration_total_(0);
Engine::hrclock::duration Engine::cycles_duration_max_(0);
int Engine::num_cycles_(0);
#endif


Engine::Engine(backend::BackendPtr backend, bool verbose)
  : _verbose(verbose)
  , _backend(backend)
  , _current_patch(NULL)
  , _current_scene(-1)
  , _current_subscene(-1)
  , _new_scene(-1)
  , _new_subscene(-1)
  , _noteon_patches(config::MAX_SIMULTANEOUS_NOTES)
  , _sustain_patches(config::MAX_SUSTAIN_PEDALS)
  , _buffer(*this)
  , _python_caller(new PythonCaller(boost::bind(&Engine::run_async, this)))
{
    // construct a patch with a single sanitize unit
    Patch::UnitExPtr sani(new units::Sanitize);
    Patch::ModulePtr mod(new Patch::Extended(sani));
    _sanitize_patch.reset(new Patch(mod));
}


Engine::~Engine()
{
    if (_backend) {
        _backend->stop();
    }

    // this needs to be gone before the engine can safely be destroyed
    _python_caller.reset();
}


void Engine::add_scene(int i, PatchPtr patch,
                       PatchPtr init_patch, PatchPtr exit_patch)
{
    if (!has_scene(i)) {
        _scenes[i] = std::vector<ScenePtr>();
    }

    _scenes[i].push_back(ScenePtr(new Scene(patch, init_patch, exit_patch)));
}


void Engine::set_processing(PatchPtr ctrl_patch,
                            PatchPtr pre_patch, PatchPtr post_patch)
{
    ASSERT(!_ctrl_patch);
    ASSERT(!_pre_patch);
    ASSERT(!_post_patch);
    _ctrl_patch = ctrl_patch;
    _pre_patch = pre_patch;
    _post_patch = post_patch;
}


void Engine::start(int initial_scene, int initial_subscene)
{
    _backend->start(
        boost::bind(&Engine::run_init, this, initial_scene, initial_subscene),
        boost::bind(&Engine::run_cycle, this)
    );
}


void Engine::run_init(int initial_scene, int initial_subscene)
{
    boost::mutex::scoped_lock lock(_process_mutex);

    // if no initial scene is specified, use the first one
    if (initial_scene == -1) {
        initial_scene = _scenes.begin()->first;
    }
    ASSERT(has_scene(initial_scene));

    _buffer.clear();

    _new_scene = initial_scene;
    _new_subscene = initial_subscene;
    process_scene_switch(_buffer);

    _backend->output_events(_buffer.begin(), _buffer.end());
}


void Engine::run_cycle()
{
    MidiEvent ev;

    while (_backend->input_event(ev))
    {
#ifdef ENABLE_BENCHMARK
        hrclock::time_point t1 = hrclock::now();
#endif

        boost::mutex::scoped_lock lock(_process_mutex);

        _buffer.clear();

        // process the event
        process(_buffer, ev);

        // handle scene switches
        process_scene_switch(_buffer);

#ifdef ENABLE_BENCHMARK
        hrclock::time_point t2 = hrclock::now();
        auto dt = t2 - t1;
        cycles_duration_total_ += dt;
        if (dt > cycles_duration_max_) {
            cycles_duration_max_ = dt;
        }
        ++num_cycles_;
#endif

        _backend->output_events(_buffer.begin(), _buffer.end());
    }
}


void Engine::run_async()
{
    if (!_backend) {
        // backend already destroyed
        return;
    }

    if (_new_scene != -1 || _new_subscene != -1) {
        boost::mutex::scoped_lock lock(_process_mutex);

        _buffer.clear();

        process_scene_switch(_buffer);

        _backend->output_events(_buffer.begin(), _buffer.end());
    }
}


std::vector<MidiEvent> Engine::process_event(MidiEvent const & ev)
{
    boost::mutex::scoped_lock lock(_process_mutex);

    std::vector<MidiEvent> v;
    Patch::EventBuffer buffer(*this);

    if (!_current_patch) {
        _current_patch = &*_scenes.find(0)->second[0]->patch;
    }

    process(buffer, ev);

    process_scene_switch(buffer);

    v.insert(v.end(), buffer.begin(), buffer.end());
    return v;
}


template <typename B>
void Engine::process(B & buffer, MidiEvent const & ev)
{
    ASSERT(buffer.empty());

    Patch * patch = get_matching_patch(ev);

    if (_ctrl_patch) {
        buffer.insert(buffer.end(), ev);
        _ctrl_patch->process(buffer);
    }

    typename B::Iterator it = buffer.insert(buffer.end(), ev);
    typename B::Range r(it, buffer.end());

    if (_pre_patch) {
        _pre_patch->process(buffer, r);
    }

    patch->process(buffer, r);

    if (_post_patch) {
        _post_patch->process(buffer, r);
    }

    _sanitize_patch->process(_buffer, r);
}


Patch * Engine::get_matching_patch(MidiEvent const & ev)
{
    // note on: store current patch
    if (ev.type == MIDI_EVENT_NOTEON) {
        _noteon_patches.insert(std::make_pair(make_notekey(ev),
                                              _current_patch));
        return _current_patch;
    }
    // note off: retrieve and remove stored patch
    else if (ev.type == MIDI_EVENT_NOTEOFF) {
        NotePatchMap::const_iterator i =
                            _noteon_patches.find(make_notekey(ev));
        if (i != _noteon_patches.end()) {
            Patch *p = i->second;
            _noteon_patches.erase(i);
            return p;
        }
    }
    // sustain pressed
    // TODO: handle half-pedal correctly
    else if (ev.type == MIDI_EVENT_CTRL &&
             ev.ctrl.param == 64 && ev.ctrl.value == 127) {
        _sustain_patches.insert(std::make_pair(make_sustainkey(ev),
                                               _current_patch));
        return _current_patch;
    }
    // sustain released
    else if (ev.type == MIDI_EVENT_CTRL &&
            ev.ctrl.param == 64 && ev.ctrl.value == 0) {
        SustainPatchMap::const_iterator i =
                                _sustain_patches.find(make_sustainkey(ev));
        if (i != _sustain_patches.end()) {
            Patch *p = i->second;
            _sustain_patches.erase(i);
            return p;
        }
    }

    // anything else: just use current patch
    return _current_patch;
}


void Engine::switch_scene(int scene, int subscene)
{
    if (scene != -1) {
        _new_scene = scene;
    }
    if (subscene != -1) {
        _new_subscene = subscene;
    }
}


template <typename B>
void Engine::process_scene_switch(B & buffer)
{
    if (_new_scene == -1 && _new_subscene == -1) {
        // nothing to do
        return;
    }

    // call python scene switch handler if we have more than one scene
    if (_scenes.size() > 1) {
        scene_switch_callback(_new_scene, _new_subscene);
    }

    // create dummy event to trigger init and exit patches
    MidiEvent dummy_ev;
    dummy_ev.type = MIDI_EVENT_DUMMY;

    // determine the actual scene and subscene number we're switching to
    int scene_num = _new_scene != -1 ? _new_scene : _current_scene;
    int subscene_num = _new_subscene != -1 ? _new_subscene : 0;

    SceneMap::const_iterator scene_it = _scenes.find(scene_num);

    // check if scene and subscene exist
    if (scene_it != _scenes.end() &&
        static_cast<int>(scene_it->second.size()) > subscene_num)
    {
        // found something...
        ScenePtr scene = scene_it->second[subscene_num];

        // if the previous (still current) scene has an exit patch, we need
        // to run that first
        if (_current_scene != -1) {
            ScenePtr prev_scene = _scenes.find(_current_scene)
                                                ->second[_current_subscene];

            if (prev_scene->exit_patch) {
                typename B::Iterator it = buffer.insert(buffer.end(),
                                                        dummy_ev);
                typename B::Range r(it, buffer.end());

                // run event through exit patch
                prev_scene->exit_patch->process(buffer, r);

                if (_post_patch) {
                    _post_patch->process(buffer, r);
                }
                _sanitize_patch->process(buffer, r);
            }
        }

        // check if the scene has an init patch
        if (scene->init_patch) {
            typename B::Iterator it = buffer.insert(buffer.end(),
                                                    dummy_ev);
            typename B::Range r(it, buffer.end());

            // run event through init patch
            scene->init_patch->process(buffer, r);

            if (_post_patch) {
                _post_patch->process(buffer, r);
            }
            _sanitize_patch->process(buffer, r);
        }

        // store pointer to patch
        _current_patch = &*scene->patch;

        // store scene and subscene numbers
        _current_scene = scene_num;
        _current_subscene = subscene_num;
    }

    // mark as done
    _new_scene = -1;
    _new_subscene = -1;
}


bool Engine::sanitize_event(MidiEvent & ev) const
{
    // FIXME: std::cout is not RT-safe!

    if (ev.port < 0 || (_backend &&
            ev.port >= static_cast<int>(_backend->num_out_ports()))) {
        // omit rather pointless warning if there are no output ports at all
        if (_verbose && _backend->num_out_ports() > 0) {
            std::cout << "invalid output port, event discarded"
                      << std::endl;
        }
        return false;
    }

    if (ev.channel < 0 || ev.channel > 15) {
        if (_verbose) {
            std::cout << "invalid channel, event discarded"
                      << std::endl;
        }
        return false;
    }

    switch (ev.type) {
        case MIDI_EVENT_NOTEON:
        case MIDI_EVENT_NOTEOFF:
            if (ev.note.note < 0 || ev.note.note > 127) {
                if (_verbose) {
                    std::cout << "invalid note number, event discarded"
                              << std::endl;
                }
                return false;
            }
            ev.note.velocity = std::max(0, std::min(127, ev.note.velocity));

            if (ev.type == MIDI_EVENT_NOTEON && ev.note.velocity < 0) {
                return false;
            }
            return true;
        case MIDI_EVENT_CTRL:
            if (ev.ctrl.param < 0 || ev.ctrl.param > 127) {
                if (_verbose) {
                    std::cout << "invalid controller number, event discarded"
                              << std::endl;
                }
                return false;
            }
            ev.ctrl.value = std::max(0, std::min(127, ev.note.velocity));
            return true;
        case MIDI_EVENT_PITCHBEND:
            ev.ctrl.value = std::max(-8192, std::min(8191, ev.ctrl.value));
            return true;
        case MIDI_EVENT_AFTERTOUCH:
            ev.ctrl.value = std::max(0, std::min(127, ev.note.velocity));
            return true;
        case MIDI_EVENT_PROGRAM:
            if (ev.ctrl.value < 0 || ev.ctrl.value > 127) {
                if (_verbose) {
                    std::cout << "invalid program number, event discarded"
                              << std::endl;
                }
                return false;
            }
            return true;
        case MIDI_EVENT_SYSEX:
            if (ev.sysex->size() < 2 || (*ev.sysex)[0] != 0xf0
                    || (*ev.sysex)[ev.sysex->size()-1] != 0xf7) {
                if (_verbose) {
                    std::cout << "invalid sysex, event discarded"
                              << std::endl;
                }
                return false;
            }
            return true;
        case MIDI_EVENT_POLY_AFTERTOUCH:
        case MIDI_EVENT_SYSCM_QFRAME:
        case MIDI_EVENT_SYSCM_SONGPOS:
        case MIDI_EVENT_SYSCM_SONGSEL:
        case MIDI_EVENT_SYSCM_TUNEREQ:
        case MIDI_EVENT_SYSRT_CLOCK:
        case MIDI_EVENT_SYSRT_START:
        case MIDI_EVENT_SYSRT_CONTINUE:
        case MIDI_EVENT_SYSRT_STOP:
        case MIDI_EVENT_SYSRT_SENSING:
        case MIDI_EVENT_SYSRT_RESET:
            return true;
        case MIDI_EVENT_DUMMY:
            return false;
        default:
            if (_verbose) {
                std::cout << "unknown event type, event discarded"
                          << std::endl;
            }
            return false;
    }
}


void Engine::output_event(MidiEvent const & ev)
{
    boost::mutex::scoped_lock lock(_process_mutex);

    _backend->output_event(ev);
}


double Engine::time()
{
#if _POSIX_TIMERS > 0
    ::timespec t;
    ::clock_gettime(CLOCK_MONOTONIC, &t);

    return t.tv_sec + 1e-9 * t.tv_nsec;
#else
    ::timeval t;
    ::gettimeofday(&t, NULL);

    return t.tv_sec + 1e-6 * t.tv_usec;
#endif
}


} // mididings
