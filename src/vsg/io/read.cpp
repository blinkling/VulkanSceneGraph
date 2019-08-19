/* <editor-fold desc="MIT License">

Copyright(c) 2018 Robert Osfield

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

</editor-fold> */

#include <vsg/io/ObjectCache.h>
#include <vsg/io/ReaderWriter_vsg.h>
#include <vsg/io/read.h>

#include <thread>
#include <iostream>
#include <chrono>
#include <condition_variable>

using namespace vsg;

ref_ptr<Object> vsg::read(const Path& filename, ref_ptr<const Options> options)
{
    ref_ptr<Object> object;
    if (options)
    {
        if (options->objectCache)
        {
            object = options->objectCache->get(filename, options);
            if (object)
            {
                return object;
            }
        }

        if (options->readerWriter)
        {
            object = options->readerWriter->read(filename, options);
        }
    }

    if (!object)
    {
        // fallback to using native ReaderWriter_vsg if extension is compatible
        auto ext = vsg::fileExtension(filename);
        if (ext == "vsga" || ext == "vsgt" || ext == "vsgb")
        {
            ReaderWriter_vsg rw;
            object = rw.read(filename, options);
        }
    }

    if (object && options && options->objectCache)
    {
        // place loaded object into the ObjectCache
        options->objectCache->add(object, filename, options);
    }

    return object;
}

struct Active : public Object
{
    Active() : active(true) {}

    std::atomic_bool active;

    explicit operator bool() const noexcept { return active; }

protected:
    virtual ~Active() {}
};


struct Latch : public Object
{
    Latch(uint32_t num) :
        count(num) {}

    void count_down()
    {
        --count;
        if (is_ready())
        {
            std::unique_lock lock(_mutex);
            cv.notify_all();
        }
    }

    bool is_ready() const
    {
        return (count==0);
    }

    void wait()
    {
        // use while loop to return immediate when latch already released
        // and to handle cases where the condition variable releases spuriously.
        while (count != 0)
        {
            std::unique_lock lock(_mutex);
            cv.wait(lock);
        }
    }

    std::atomic_uint count;
    std::mutex _mutex;
    std::condition_variable cv;

protected:
    virtual ~Latch() {}
};

struct Operation : public Object
{
    virtual void run() = 0;
};


struct OperationQueue : public Object
{
    std::mutex _mutex;
    std::condition_variable _cv;
    std::list<ref_ptr<Operation>> _queue;
    ref_ptr<Active> _active;

    void add(ref_ptr<Operation> operation)
    {
        std::unique_lock lock(_mutex);
        _queue.emplace_back(operation);
        _cv.notify_one();
    }

    template<typename Iterator>
    void add(Iterator begin, Iterator end)
    {
        size_t numAdditions = 0;
        std::unique_lock lock(_mutex);
        for(auto itr = begin; itr != end; ++itr)
        {
            _queue.emplace_back(*itr);
            ++numAdditions;
        }

        if (numAdditions==1) _cv.notify_one();
        else if (numAdditions>1) _cv.notify_all();
    }

    ref_ptr<Operation> take()
    {
        std::unique_lock lock(_mutex);

        if (_queue.empty()) return {};

        ref_ptr<Operation> operation = _queue.front();

        _queue.erase(_queue.begin());

        return operation;
    }

    ref_ptr<Operation> take_when_avilable()
    {
        std::chrono::duration waitDuration = std::chrono::milliseconds(100);

        std::unique_lock lock(_mutex);

        // wait to the conditional variable signals that an operation has been added
        while (_queue.empty() && *_active)
        {
            //std::cout<<"Waiting on condition variable"<<std::endl;
            _cv.wait_for(lock, waitDuration);
        }

        // if the threads we are associated with should no longer running go for a quick exit and return nothing.
        if (!*_active)
        {
            return {};
        }

        // remove and return the head of the queue
        ref_ptr<Operation> operation = _queue.front();
        _queue.erase(_queue.begin());
        return operation;
    }
};


struct ReadOperation : public Operation
{
    ReadOperation(const Path& f, ref_ptr<const Options> opt, ref_ptr<Object>& obj, ref_ptr<Latch> l) :
        filename(f),
        options(opt),
        object(obj),
        latch(l) {}

    void run() override
    {
        object = vsg::read(filename, options);
        latch->count_down();
    }

    Path filename;
    ref_ptr<const Options> options;
    ref_ptr<Object>& object;
    ref_ptr<Latch> latch;
};

PathObjects vsg::read(const Paths& filenames, ref_ptr<const Options> options)
{
    enum ThreadingModel
    {
        SingleThreaded,
        ThreadPerLoad,
        ThreadPool
    };

    //ThreadingModel threadingModel = SingleThreaded;
    //ThreadingModel threadingModel = ThreadPerLoad;
    ThreadingModel threadingModel = ThreadPool;

// start of thread pool setup
    ref_ptr<OperationQueue> operationQueue;
    ref_ptr<Active> active;
    std::vector<std::thread> poolThreads;

    if (threadingModel == ThreadPool)
    {
        operationQueue = new OperationQueue;
        active = new Active;

        operationQueue->_active = active;

        auto run = [operationQueue, active]()
        {
            while(*active)
            {
                ref_ptr<Operation> operation = operationQueue->take_when_avilable();
                if (operation) operation->run();
            }
        };

        size_t numThreads = 8;
        for(size_t i=0; i<numThreads; ++i)
        {
            poolThreads.emplace_back(std::thread(run));
        }
    }
// end of thread pool setup

    auto before_vsg_load = std::chrono::steady_clock::now();

    PathObjects entries;

    switch(threadingModel)
    {
        case(SingleThreaded) :
        {
            for(auto& filename : filenames)
            {
                if (!filename.empty())
                {
                    entries[filename] = vsg::read(filename, options);
                }
                else
                {
                    entries[filename] = nullptr;
                }
            }
            break;
        }

        case(ThreadPerLoad) :
        {
            auto readFile = [](const Path& filename, ref_ptr<const Options> opt, ref_ptr<Object>& object)
            {
                object = vsg::read(filename, opt);
            };

            for(auto& filename : filenames)
            {
                entries[filename] = nullptr;
            }

            std::vector<std::thread> threads;
            for(auto& [filename, object] : entries)
            {
                threads.emplace_back(std::thread(readFile, std::ref(filename), options, std::ref(object)));
            }

            for(auto& thread : threads)
            {
                thread.join();
            }
            break;
        }

        case(ThreadPool) :
        {
            // set up the entries container for operations to write to.
            for(auto& filename : filenames)
            {
                entries[filename] = nullptr;
            }

            // use latch to syncronize this thread with the file reading threads
            ref_ptr<Latch> latch(new Latch(filenames.size()));

            // add operations
            for(auto& [filename, object] : entries)
            {
                operationQueue->add(ref_ptr<Operation>(new ReadOperation(filename, options, object, latch)));
            }

            // use this thread to read the files as well
            while(ref_ptr<Operation> operation = operationQueue->take())
            {
                operation->run();
            }

            latch->wait();
            break;
        }
    }

    auto vsg_loadTime = std::chrono::duration<double, std::chrono::milliseconds::period>(std::chrono::steady_clock::now() - before_vsg_load).count();
    std::cout<<"After batch load() time =  "<<vsg_loadTime<<std::endl;

// clean up thread pool
    if (threadingModel == ThreadPool)
    {
        std::cout<<"\nWork COMPLETED time to stop threads"<<std::endl;

        active->active = false;

        for(auto& thread : poolThreads)
        {
            thread.join();
        }
    }
// end of clean of thread pool

    return entries;
}
